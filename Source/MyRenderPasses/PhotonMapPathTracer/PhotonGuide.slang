#include "Utils/Math/MathConstants.slangh"

import PhotonMap;
import Rendering.Materials.IMaterialInstance;
import Utils.Math.ShadingFrame;
import Utils.Sampling.SampleGenerator;

static const uint kGuideAzimuthBins = 8;
static const uint kGuideRadialBins = 4;
static const uint kGuideBinCount = kGuideAzimuthBins * kGuideRadialBins;
static const float kTwoPi = 2.f * M_PI;
static const float3 kLuminance = float3(0.2126f, 0.7152f, 0.0722f);

// 论文的单位方形参数化。u 是投影半径的平方，v 是方位角。
float3 cosineDirection(float2 uv)
{
    const float phi = uv.y * kTwoPi;
    return float3(sqrt(uv.x) * float2(cos(phi), sin(phi)), sqrt(1.f - uv.x));
}

uint directionBin(float3 local)
{
    const float u = saturate(1.f - local.z * local.z);
    const float v = frac(atan2(local.y, local.x) / kTwoPi + 1.f);
    const uint row = min(uint(u * kGuideRadialBins), kGuideRadialBins - 1u);
    const uint column = min(uint(v * kGuideAzimuthBins), kGuideAzimuthBins - 1u);
    return row * kGuideAzimuthBins + column;
}

float2 binPoint(uint bin, float2 offset)
{
    return (float2(bin / kGuideAzimuthBins, bin % kGuideAzimuthBins) + offset) / float2(kGuideRadialBins, kGuideAzimuthBins);
}

struct PhotonGuide
{
    ShadingFrame frame;
    float cdf[kGuideBinCount];
    float total;

    float pdf(float3 direction)
    {
        const float3 local = frame.toLocal(direction);
        if (!(total > 0.f) || local.z <= 0.f)
            return 0.f;
        const uint bin = directionBin(local);
        const float mass = cdf[bin] - (bin == 0u ? 0.f : cdf[bin - 1u]);
        return (mass / total) * (float(kGuideBinCount) / M_PI) * local.z;
    }

    float3 sample(inout SampleGenerator sg)
    {
        const float target = min(sampleNext1D(sg) * total, asfloat(asuint(total) - 1u));
        uint lower = 0u;
        uint upper = kGuideBinCount - 1u;
        while (lower < upper)
        {
            const uint mid = (lower + upper) / 2u;
            if (cdf[mid] > target)
                upper = mid;
            else
                lower = mid + 1u;
        }
        const float2 offset = clamp(sampleNext2D(sg), 1e-6f, 1.f - 1e-6f);
        return frame.fromLocal(cosineDirection(binPoint(lower, offset)));
    }
};

PhotonGuide gatherPhotonGuide(
    const ShadingData sd,
    const IMaterialInstance mi,
    PhotonGrid grid,
    StructuredBuffer<uint> cellCounts,
    StructuredBuffer<Photon> photons,
    uint minPhotons,
    inout SampleGenerator sg
)
{
    PhotonGuide guide = {};
    const float3 normal = sd.getOrientedFaceNormal();
    bool frameValid;
    guide.frame = ShadingFrame::createSafe(normal, sd.tangentW, frameValid);

    float3 binFlux[kGuideBinCount] = {};
    uint gathered = 0u;
    const uint3 first = grid.cell(sd.posW - grid.radius);
    const uint3 last = grid.cell(sd.posW + grid.radius);
    const float maxPlaneDistance = 0.25f * grid.radius;
    const float invRadiusSq = 1.f / (grid.radius * grid.radius);

    for (uint z = first.z; z <= last.z; ++z)
    {
        for (uint y = first.y; y <= last.y; ++y)
        {
            for (uint x = first.x; x <= last.x; ++x)
            {
                const uint cell = grid.index(uint3(x, y, z));
                const uint count = min(cellCounts[cell], kPhotonCellCapacity);
                for (uint slot = 0u; slot < count; ++slot)
                {
                    const Photon photon = photons[cell * kPhotonCellCapacity + slot];
                    const float3 delta = photon.position - sd.posW;
                    const float weight = 1.f - dot(delta, delta) * invRadiusSq;
                    if (weight <= 0.f || dot(photon.normal, normal) < 0.75f || abs(dot(delta, normal)) > maxPlaneDistance)
                        continue;
                    const float3 local = guide.frame.toLocal(photon.directionToLight);
                    if (local.z <= 0.f)
                        continue;
                    binFlux[directionBin(local)] += photon.flux * weight;
                    ++gathered;
                }
            }
        }
    }
    if (gathered < minPhotons)
        return guide;

    for (uint bin = 0u; bin < kGuideBinCount; ++bin)
    {
        if (any(binFlux[bin] > 0.f))
        {
            // 光子通量已包含入射余弦。Falcor 的 eval 返回 f * cos，求分布时去掉该余弦。
            const float3 local = cosineDirection(binPoint(bin, float2(0.5f)));
            const float3 brdf = mi.eval(sd, guide.frame.fromLocal(local), sg) / local.z;
            const float weight = dot(binFlux[bin] * max(brdf, float3(0.f)), kLuminance);
            if (isfinite(weight))
                guide.total += weight;
        }
        guide.cdf[bin] = guide.total;
    }
    if (!isfinite(guide.total))
        guide.total = 0.f;
    return guide;
}
