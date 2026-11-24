__exported import CacheTypes;
import CosineLobe;
#include "Utils/Math/MathConstants.slangh"
import Scene.Scene;
import Scene.Material.MaterialTypes;
import Rendering.Materials.IMaterialInstance;
import Utils.Color.ColorHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;

cbuffer GuideCB
{
    CacheGrid gGrid;
    uint gCacheReady;
    uint gSearchNeighbors;
    uint gGuidedBounces;
    float gGuideMix;
    float gMinRoughness;
}

StructuredBuffer<CacheRecord> gRecords;
RWStructuredBuffer<CacheSample> gSamples;
RWStructuredBuffer<uint> gSampleCounts;

struct GuideLobe
{
    float3 direction;
    uint exponent;
    float weight;
    float normalization;
};

struct CacheGuide
{
    float3 normal;
    uint eligible;
    uint cell;
    uint count;
    float total;
    float mixProbability;
    GuideLobe lobes[kQueryLobes];

    float pdf(float3 direction)
    {
        if (!(total > 0.f) || dot(normal, direction) <= 0.f)
            return 0.f;
        float result = 0.f;
        for (uint i = 0u; i < count; ++i)
            result += lobes[i].weight * lobes[i].normalization *
                pow(saturate(dot(lobes[i].direction, direction)), float(lobes[i].exponent));
        return result / total;
    }

    float3 sample(inout SampleGenerator sg)
    {
        const float selector = sampleNext1D(sg) * total;
        float cdf = 0.f;
        uint selected = count - 1u;
        for (uint i = 0u; i < count; ++i)
        {
            cdf += lobes[i].weight;
            if (selector < cdf)
            {
                selected = i;
                break;
            }
        }
        const GuideLobe lobe = lobes[selected];
        return sampleClippedCosineLobe(lobe.direction, normal, lobe.exponent, sg);
    }
};

CacheGuide gatherCacheGuide(const HitInfo hit, const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg)
{
    CacheGuide guide = {};
    if (hit.getType() != HitType::Triangle)
        return guide;
    const MaterialHeader header = gScene.materials.getMaterialHeader(sd.materialID);
    const MaterialType type = header.getMaterialType();
    const uint lobes = mi.getLobeTypes(sd);
    if (header.getAlphaMode() != AlphaMode::Opaque || (type != MaterialType::Standard && type != MaterialType::PBRTDiffuse) ||
        (lobes & uint(LobeType::Reflection)) == 0u || (lobes & (uint(LobeType::Transmission) | uint(LobeType::Delta))) != 0u ||
        mi.getProperties(sd).roughness < gMinRoughness)
        return guide;

    guide.normal = sd.getOrientedFaceNormal();
    guide.eligible = 1u;
    const uint3 coordinate = cellCoordinate(gGrid, sd.posW);
    guide.cell = cellIndex(gGrid, coordinate);
    if (gCacheReady == 0u)
        return guide;

    uint indices[kQueryLobes];
    float distances[kQueryLobes];
    for (uint i = 0u; i < kQueryLobes; ++i)
    {
        indices[i] = 0u;
        distances[i] = FLT_MAX;
    }
    const int range = gSearchNeighbors != 0u ? 1 : 0;
    for (int z = -range; z <= range; ++z)
    for (int y = -range; y <= range; ++y)
    for (int x = -range; x <= range; ++x)
    {
        const int3 neighbor = int3(coordinate) + int3(x, y, z);
        if (any(neighbor < 0) || any(neighbor >= int(gGrid.resolution)))
            continue;
        const uint first = cellIndex(gGrid, uint3(neighbor)) * kLobesPerCell;
        for (uint j = 0u; j < kLobesPerCell; ++j)
        {
            const CacheRecord record = gRecords[first + j];
            const float3 delta = record.position - sd.posW;
            const float distance = dot(delta, delta);
            if (record.count == 0u || !(record.radiance > 0.f) || distance > gGrid.radiusSquared ||
                dot(record.direction, guide.normal) <= 0.f)
                continue;
            uint farthest = 0u;
            for (uint i = 1u; i < kQueryLobes; ++i)
                if (distances[i] > distances[farthest])
                    farthest = i;
            if (distance < distances[farthest])
            {
                indices[farthest] = first + j;
                distances[farthest] = distance;
            }
        }
    }

    for (uint i = 0u; i < kQueryLobes; ++i)
    {
        if (distances[i] == FLT_MAX)
            continue;
        const CacheRecord record = gRecords[indices[i]];
        const float weight = luminance(max(mi.eval(sd, record.direction, sg), 0.f)) * record.radiance;
        if (!(weight > 0.f && isfinite(weight)))
            continue;
        GuideLobe lobe;
        lobe.direction = record.direction;
        lobe.exponent = uint(ceil(record.exponent));
        lobe.weight = weight;
        // 同一平面直接复用精确归一化常数，法线有任何差异时重新计算。
        lobe.normalization = record.normalization;
        [branch]
        if (any(guide.normal != record.normal))
            lobe.normalization = float(lobe.exponent + 1u) /
                (M_2PI * clippedLobeMass(lobe.exponent, dot(guide.normal, lobe.direction)));
        guide.lobes[guide.count++] = lobe;
        guide.total += weight;
    }
    if (guide.total > 0.f && isfinite(guide.total))
        guide.mixProbability = gGuideMix;
    return guide;
}

void storeCacheSample(CacheSample sample, float3 incoming)
{
    if (sample.valid == 0u || !all(isfinite(incoming)) || dot(sample.normal, sample.direction) <= 0.f)
        return;
    sample.radiance = luminance(max(incoming, 0.f));
    const uint cell = cellIndex(gGrid, cellCoordinate(gGrid, sample.position));
    uint slot;
    InterlockedAdd(gSampleCounts[cell], 0u, slot);
    while (slot < kSamplesPerCell)
    {
        uint observed;
        InterlockedCompareExchange(gSampleCounts[cell], slot, slot + 1u, observed);
        if (observed == slot)
        {
            gSamples[cell * kSamplesPerCell + slot] = sample;
            return;
        }
        slot = observed;
    }
}
