import PhotonMap;
import SurfaceTracing;
import Rendering.Lights.EmissiveLightSamplerInterface;
import Rendering.Lights.EmissiveUniformSampler;
import Rendering.Lights.LightHelpers;
import MyRenderPasses.Shared.EmissiveLightHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;

#if PHOTON_GUIDING
import PhotonGuide;
#endif

cbuffer CB
{
    uint gFrameIndex;
    uint gPRNGDimension;
    uint gMaxBounces;
    uint gHasView;
    uint gUseAnalyticLights;
    uint gUseEmissiveLights;
    uint gUseEnvLight;
    uint gUseEnvBackground;
#if PHOTON_GUIDING
    PhotonGrid gGrid;
    uint gMinPhotons;
    float gGuideMix;
    uint gGuidedBounces;
#endif
}

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW;
RWTexture2D<float4> gOutputColor;
#if PHOTON_GUIDING
StructuredBuffer<uint> gCellCounts;
StructuredBuffer<Photon> gPhotons;
#endif

struct ScatterDistribution
{
#if PHOTON_GUIDING
    PhotonGuide guide;
    float mixProbability;
#endif

    float pdf(const ShadingData sd, const IMaterialInstance mi, float3 direction)
    {
        const float bsdfPdf = abs(mi.evalPdf(sd, direction));
#if PHOTON_GUIDING
        if (mixProbability > 0.f)
            return lerp(bsdfPdf, guide.pdf(direction), mixProbability);
#endif
        return bsdfPdf;
    }

    bool sample(const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg, out BSDFSample result)
    {
#if PHOTON_GUIDING
        if (mixProbability > 0.f && sampleNext1D(sg) < mixProbability)
        {
            result = {};
            result.wo = guide.sample(sg);
            result.pdf = pdf(sd, mi, result.wo);
            if (!(result.pdf > 0.f))
                return false;
            result.weight = mi.eval(sd, result.wo, sg) / result.pdf;
            return any(result.weight > 0.f);
        }
#endif
        BSDFSample bsdfSample = {};
        if (!mi.sample(sd, sg, bsdfSample))
        {
            result = {};
            return false;
        }
        result = bsdfSample;

#if PHOTON_GUIDING
        if (mixProbability > 0.f)
        {
            if (result.isLobe(LobeType::Delta))
            {
                // 离散事件只由 BSDF 分支生成，其质量乘以分支概率。
                result.weight /= 1.f - mixProbability;
            }
            else
            {
                const float bsdfPdf = result.pdf;
                result.pdf = lerp(bsdfPdf, guide.pdf(result.wo), mixProbability);
                if (!(result.pdf > 0.f))
                    return false;
                result.weight *= bsdfPdf / result.pdf;
            }
        }
#endif
        return any(result.weight > 0.f);
    }
};

float powerHeuristic(float pdf, float otherPdf)
{
    const float ratio = otherPdf / pdf;
    return 1.f / (1.f + ratio * ratio);
}

float3 sampleAnalyticLight(const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg)
{
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0u)
        return float3(0.f);
    const uint lightIndex = min(uint(sampleNext1D(sg) * lightCount), lightCount - 1u);
    AnalyticLightSample light;
    if (!sampleLight(sd.posW, gScene.getLight(lightIndex), sg, light))
        return float3(0.f);

    const float3 scatter = mi.eval(sd, light.dir, sg);
    if (!any(scatter > 0.f))
        return float3(0.f);
    const float3 origin = sd.computeRayOrigin(dot(sd.getOrientedFaceNormal(), light.dir) >= 0.f);
    SceneRayQuery<1> query;
    if (!query.traceVisibilityRay(Ray(origin, light.dir, 0.f, light.distance)))
        return float3(0.f);

    return scatter * light.Li * float(lightCount);
}

float3 sampleEmissiveLight(
    const ShadingData sd,
    const IMaterialInstance mi,
    const ScatterDistribution distribution,
    inout SampleGenerator sg
)
{
    EmissiveUniformSampler sampler = {};
    TriangleLightSample light;
    if (!sampler.sampleLight(sd.posW, sd.getOrientedFaceNormal(), false, sg, light))
        return float3(0.f);
    if (!(light.pdf > 0.f && isfinite(light.pdf)) || !any(light.Le > 0.f))
        return float3(0.f);
    if (isMaskedLightSample(light))
        return float3(0.f);

    const float3 scatter = mi.eval(sd, light.dir, sg);
    if (!any(scatter > 0.f))
        return float3(0.f);
    const float3 origin = sd.computeRayOrigin(dot(sd.getOrientedFaceNormal(), light.dir) >= 0.f);
    const float3 segment = computeRayOrigin(light.posW, light.normalW) - origin;
    if (dot(segment, light.dir) <= 0.f)
        return float3(0.f);
    const float distance = length(segment);
    SceneRayQuery<1> query;
    if (!query.traceVisibilityRay(Ray(origin, segment / distance, 0.f, distance)))
        return float3(0.f);

    const float scatterPdf = distribution.pdf(sd, mi, light.dir);
    return scatter * light.Le * (powerHeuristic(light.pdf, scatterPdf) / light.pdf);
}

float emissiveHitWeight(const HitInfo hit, const ShadingData sd, float3 prevPosition, float scatterPdf)
{
    if (!(scatterPdf > 0.f) || hit.getType() != HitType::Triangle)
        return 1.f;
    const TriangleHit triangle = hit.getTriangleHit();
    const uint index = gScene.lightCollection.getTriangleIndex(triangle.instanceID, triangle.primitiveIndex);
    if (index == LightCollection::kInvalidIndex || gScene.lightCollection.getActiveTriangleIndex(index) == LightCollection::kInvalidIndex)
        return 1.f;

    TriangleLightHit lightHit;
    lightHit.triangleIndex = index;
    lightHit.posW = sd.posW;
    lightHit.normalW = gScene.lightCollection.getTriangle(index).normal;
    EmissiveUniformSampler sampler = {};
    const float lightPdf = sampler.evalPdf(prevPosition, float3(0.f), false, lightHit);
    return powerHeuristic(scatterPdf, lightPdf);
}

float3 traceCameraPath(uint2 pixel, uint2 dim)
{
    const float3 direction = gHasView != 0u ? -gViewW[pixel].xyz : gScene.camera.computeRayPinhole(pixel, dim).dir;
    HitInfo hit = HitInfo(gVBuffer[pixel]);
    if (!hit.isValid())
        return gUseEnvBackground != 0u ? gScene.envMap.eval(direction) : float3(0.f);

    SampleGenerator sg = SampleGenerator(pixel, gFrameIndex);
    for (uint i = 0u; i < gPRNGDimension; ++i)
        sampleNext1D(sg);

    Ray ray = Ray(gScene.camera.getPosition(), direction);
    float3 radiance = float3(0.f);
    float3 throughput = float3(1.f);
    float3 prevPosition = float3(0.f);
    float prevScatterPdf = 0.f;
    SceneRayQuery<1> query;

    // 在最后一段仍保留发光面或环境的命中贡献，以配对上一顶点的直接光 MIS。
    for (uint depth = 0u; depth <= gMaxBounces + 1u; ++depth)
    {
        const ShadingData sd = loadSurface(hit, ray);
        let mi = gScene.materials.getMaterialInstance(sd, ExplicitLodTextureSampler(0.f));
        if (depth == 0u || gUseEmissiveLights != 0u)
        {
            float3 emission = mi.getProperties(sd).emission;
            if (any(emission > 0.f))
            {
                if (gScene.materials.hasLightProfile(sd.materialID))
                    emission *= gScene.materials.evalLightProfile(dot(sd.faceN, sd.V));
                radiance += throughput * emission * emissiveHitWeight(hit, sd, prevPosition, prevScatterPdf);
            }
        }
        if (depth > gMaxBounces)
            break;

        const uint lobes = mi.getLobeTypes(sd);
        ScatterDistribution distribution = {};
#if PHOTON_GUIDING
        if (depth < min(gGuidedBounces, gMaxBounces) && (lobes & uint(LobeType::DiffuseReflection)) != 0u &&
            hit.getType() != HitType::Curve)
        {
            distribution.guide = gatherPhotonGuide(sd, mi, gGrid, gCellCounts, gPhotons, gMinPhotons, sg);
            if (distribution.guide.total > 0.f)
                distribution.mixProbability = gGuideMix;
        }
#endif
        const bool sampleEmissive = gUseEmissiveLights != 0u && (lobes & uint(LobeType::NonDelta)) != 0u;
        if (gUseAnalyticLights != 0u)
            radiance += throughput * sampleAnalyticLight(sd, mi, sg);
        if (sampleEmissive)
            radiance += throughput * sampleEmissiveLight(sd, mi, distribution, sg);

        BSDFSample scatter;
        if (!distribution.sample(sd, mi, sg, scatter))
            break;
        throughput *= scatter.weight;
        if (!any(throughput > 0.f) || !all(isfinite(throughput)))
            break;

        prevPosition = sd.posW;
        prevScatterPdf = sampleEmissive && !scatter.isLobe(LobeType::Delta) ? scatter.pdf : 0.f;
        ray = Ray(scatterOrigin(hit, sd, scatter.wo), scatter.wo);
        float hitT;
        hit = query.traceRay(ray, hitT);
        if (!hit.isValid())
        {
            if (gUseEnvLight != 0u)
                radiance += throughput * gScene.envMap.eval(ray.dir);
            break;
        }
    }
    return radiance;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint2 dim;
    gOutputColor.GetDimensions(dim.x, dim.y);
    if (any(tid.xy >= dim))
        return;
    gOutputColor[tid.xy] = float4(traceCameraPath(tid.xy, dim), 1.f);
}
