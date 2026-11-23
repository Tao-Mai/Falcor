/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/

// 每像素生成一条路径。缓存用于主命中处的引导采样和控制变量估计。

#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Math.MathHelpers;
import Utils.Geometry.GeometryHelpers;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.LightHelpers;
import Rendering.Lights.EmissiveLightSamplerInterface;
import Rendering.Lights.EmissiveUniformSampler;
import Scene.Material.MaterialTypes;
import PG5DTreeCache;

cbuffer CB
{
    uint gFrameCount;    // Frame sample index, including the configured seed offset.
    uint gPRNGDimension; // First available PRNG dimension.
}

struct GuideContext
{
    uint cellIndex;
    uint hasEnoughSamples;
    ShadingFrame frame;
    float minCosine;
    float mix;
    float cvStrength;
    float3 albedo;
    float3 cvIntegral;
};

#if USE_RADIANCE_CACHE
cbuffer GuideCB
{
    float3 gGridMin;
    uint gGridResolution;
    float3 gGridInvExtent;
    uint gCacheReady;
    uint2 gDirectionResolution;
    float gGuideMix;
    float gCvStrength;
    float gMinRoughness;
}

RWByteAddressBuffer gTrainingBins;
RWBuffer<uint> gTrainingCounts;
StructuredBuffer<RadianceCacheBin> gCacheBins;
StructuredBuffer<RadianceCacheCell> gCacheCells;

GuideContext getGuideContext(const HitInfo hit, const ShadingData sd, const IMaterialInstance mi)
{
    GuideContext guide = {};
    guide.cellIndex = kInvalidCacheIndex;
    if (hit.getType() != HitType::Triangle)
        return guide;

    const MaterialHeader header = gScene.materials.getMaterialHeader(sd.materialID);
    const MaterialType materialType = header.getMaterialType();
    if (header.getAlphaMode() != AlphaMode::Opaque || (materialType != MaterialType::Standard && materialType != MaterialType::PBRTDiffuse))
        return guide;

    const uint lobes = mi.getLobeTypes(sd);
    if ((lobes & uint(LobeType::DiffuseReflection)) == 0 || (lobes & (uint(LobeType::Transmission) | uint(LobeType::Delta))) != 0)
        return guide;

    const BSDFProperties props = mi.getProperties(sd);
    const float3 normal = props.guideNormal;
    const float alignment = dot(normal, sd.getOrientedFaceNormal());
    if (alignment <= 0.f || dot(normal, sd.V) <= kMinCosTheta || props.roughness < gMinRoughness ||
        !any(props.diffuseReflectionAlbedo > 0.f))
        return guide;

    // 锥内方向均位于几何反射侧，也处在材质 PDF 的连续支持域。
    guide.minCosine = sqrt(saturate(1.f - alignment * alignment)) + 1e-4f;
    if (guide.minCosine >= 1.f)
        return guide;

    const float3 uvw = (sd.posW - gGridMin) * gGridInvExtent;
    if (!all(isfinite(uvw)))
        return guide;
    const uint3 coord = min(uint3(saturate(uvw) * gGridResolution), gGridResolution - 1u);
    guide.cellIndex = (coord.z * gGridResolution + coord.y) * gGridResolution + coord.x;
    guide.frame.N = normal;
    branchlessONB(normal, guide.frame.T, guide.frame.B);
    guide.albedo = props.diffuseReflectionAlbedo;
    if (gCacheReady != 0)
    {
        const RadianceCacheCell cell = gCacheCells[guide.cellIndex];
        guide.hasEnoughSamples = cell.hasEnoughSamples;
        if (cell.hasEnoughSamples != 0 && any(cell.meanRadiance > 0.f))
        {
            guide.mix = gGuideMix;
            guide.cvStrength = gCvStrength;
            guide.cvIntegral = guide.albedo * cell.meanRadiance * (1.f - guide.minCosine * guide.minCosine);
        }
    }
    return guide;
}

uint getGuideBinCount()
{
    return gDirectionResolution.x * gDirectionResolution.y;
}

// q 是截断余弦锥的立体角 PDF，各参数箱在 q 下的概率均为 1 / B。
uint getGuideBin(const GuideContext guide, const float3 direction, out float q)
{
    q = 0.f;
    const float3 local = guide.frame.toLocal(direction);
    if (local.z < guide.minCosine)
        return kInvalidCacheIndex;

    const float lower = guide.minCosine * guide.minCosine;
    const float u = any(local.xy != 0.f) ? frac(atan2(local.y, local.x) / M_2PI) : 0.f;
    const float v = (local.z * local.z - lower) / (1.f - lower);
    const uint2 coord = min(uint2(saturate(float2(u, v)) * gDirectionResolution), gDirectionResolution - 1u);
    q = local.z / (M_PI * (1.f - lower));
    return guide.cellIndex * getGuideBinCount() + coord.y * gDirectionResolution.x + coord.x;
}

float3 sampleGuide(const GuideContext guide, inout SampleGenerator sg)
{
    const uint firstBin = guide.cellIndex * getGuideBinCount();
    const float selector = sampleNext1D(sg);
    uint low = 0u;
    uint high = getGuideBinCount() - 1u;
    while (low < high)
    {
        const uint mid = (low + high) / 2u;
        if (selector < gCacheBins[firstBin + mid].cdf)
            high = mid;
        else
            low = mid + 1u;
    }

    const uint2 bin = uint2(low % gDirectionResolution.x, low / gDirectionResolution.x);
    const float2 uv = (float2(bin) + sampleNext2D(sg)) / float2(gDirectionResolution);
    const float phi = uv.x * M_2PI;
    const float cosTheta = sqrt(lerp(guide.minCosine * guide.minCosine, 1.f, uv.y));
    const float sinTheta = sqrt(saturate(1.f - cosTheta * cosTheta));
    return guide.frame.fromLocal(float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}

float mixScatterPdf(const GuideContext guide, const uint bin, const float q, const float bsdfPdf)
{
    float guidePdf = 0.f;
    if (bin != kInvalidCacheIndex)
    {
        const uint binCount = getGuideBinCount();
        const float previous = bin == guide.cellIndex * binCount ? 0.f : gCacheBins[bin - 1u].cdf;
        guidePdf = (gCacheBins[bin].cdf - previous) * binCount * q;
    }
    return lerp(bsdfPdf, guidePdf, guide.mix);
}

// 使用 32 位整数 CAS 累加浮点，供 Vulkan 和 D3D12 共用。
void addTrainingValue(const uint offset, const float value)
{
    if (value == 0.f)
        return;

    uint expected = 0u;
    [allow_uav_condition]
    for (;;)
    {
        uint observed;
        gTrainingBins.InterlockedCompareExchange(offset, expected, asuint(asfloat(expected) + value), observed);
        if (expected == observed)
            return;
        expected = observed;
    }
}

void trainGuide(const GuideContext guide, const uint bin, const float3 radiance, const float weight)
{
    const float3 weighted = radiance * weight;
    if (!(weight > 0.f && isfinite(weight) && all(isfinite(weighted))))
        return;

    const uint offset = bin * kTrainingBinStride;
    addTrainingValue(offset,     weighted.x);
    addTrainingValue(offset + 4, weighted.y);
    addTrainingValue(offset + 8, weighted.z);
    addTrainingValue(offset + 12, weight);
    if (!guide.hasEnoughSamples)
        InterlockedAdd(gTrainingCounts[guide.cellIndex], 1u);
}
#endif

// Inputs
Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW; // Optional

// Outputs
RWTexture2D<float4> gOutputColor;

// Static configuration based on defines set from the host.
#define is_valid(name) (is_valid_##name != 0)
static const uint kMaxBounces = MAX_BOUNCES;
static const bool kComputeDirect = COMPUTE_DIRECT;
static const bool kUseImportanceSampling = USE_IMPORTANCE_SAMPLING;
static const bool kUseAnalyticLights = USE_ANALYTIC_LIGHTS;
static const bool kUseEmissiveLights = USE_EMISSIVE_LIGHTS;
static const bool kUseEnvLight = USE_ENV_LIGHT;
static const bool kUseEnvBackground = USE_ENV_BACKGROUND;
static const float3 kDefaultBackgroundColor = float3(0, 0, 0);
static const float kRayTMax = FLT_MAX;

/**
 * Payload for shadow ray.
 */
struct ShadowRayData
{
    bool visible;
};

// 散射射线载荷。缓存分支还会记录首段命中的发光项。
struct ScatterRayData
{
    float3 radiance;  ///< Accumulated outgoing radiance from path.
    bool terminated;  ///< Set to true when path is terminated.
    float3 thp;       ///< Current path throughput. This is updated at each path vertex.
    uint pathLength;  ///< Path length in number of path segments (0 at origin, 1 at first secondary hit, etc.). Max 2^31.
    float3 origin;    ///< Next path segment origin.
    float3 direction; ///< Next path segment direction.
    float3 scatterPositionW; ///< 上一个散射点的原始位置，用于求灯光 PDF。
    float scatterPdf; ///< 用于 MIS 的立体角 PDF。零表示该散射事件保留完整命中贡献。

    SampleGenerator sg; ///< Per-ray state for the sample generator (up to 16B).
#if USE_RADIANCE_CACHE
    float3 firstHitEmission;
    bool splitFirstHitEmission;
#endif

    /**
     * Initializes ray payload with default parameters.
     */
    __init(SampleGenerator sg)
    {
        this.terminated = false;
        this.pathLength = 0;
        this.radiance = float3(0, 0, 0);
        this.thp = float3(1, 1, 1);
        this.origin = float3(0, 0, 0);
        this.direction = float3(0, 0, 0);
        this.scatterPositionW = float3(0.f);
        this.scatterPdf = 0.f;
        this.sg = sg;
#if USE_RADIANCE_CACHE
        this.firstHitEmission = float3(0.f);
        this.splitFirstHitEmission = false;
#endif
    }
};

/**
 * Setup ShadingData based on loaded vertex/material attributes for a hit point.
 * @param[in] hit Hit information.
 * @param[in] rayOrigin Ray origin.
 * @param[in] rayDir Normalized ray direction.
 * @return ShadingData struct.
 */
ShadingData loadShadingData(const HitInfo hit, const float3 rayOrigin, const float3 rayDir)
{
    VertexData v = {};
    uint materialID = {};

#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_TRIANGLE_MESH)
    if (hit.getType() == HitType::Triangle)
    {
        const TriangleHit triangleHit = hit.getTriangleHit();
        v = gScene.getVertexData(triangleHit);
        materialID = gScene.getMaterialID(triangleHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_DISPLACED_TRIANGLE_MESH)
    if (hit.getType() == HitType::DisplacedTriangle)
    {
        const DisplacedTriangleHit displacedTriangleHit = hit.getDisplacedTriangleHit();
        v = gScene.getVertexData(displacedTriangleHit, -rayDir);
        materialID = gScene.getMaterialID(displacedTriangleHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_CURVE)
    if (hit.getType() == HitType::Curve)
    {
        const CurveHit curveHit = hit.getCurveHit();
        v = gScene.getVertexDataFromCurve(curveHit);
        materialID = gScene.getMaterialID(curveHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_SDF_GRID)
    if (hit.getType() == HitType::SDFGrid)
    {
        const SDFGridHit sdfGridHit = hit.getSDFGridHit();
        v = gScene.getVertexDataFromSDFGrid(sdfGridHit, rayOrigin, rayDir);
        materialID = gScene.getMaterialID(sdfGridHit.instanceID);
    }
#endif

    ShadingData sd = gScene.materials.prepareShadingData(v, materialID, -rayDir);

    return sd;
}

/**
 * Returns the primary ray's direction.
 */
float3 getPrimaryRayDir(uint2 launchIndex, uint2 launchDim, const Camera camera)
{
    if (is_valid(gViewW))
    {
        // If we have the view vector bound as a buffer, just fetch it. No need to compute anything.
        return -gViewW[launchIndex].xyz;
    }
    else
    {
        // Compute the view vector. This must exactly match what the G-buffer pass is doing (jitter etc.).
        // Note that we do not take depth-of-field into account as it would require exactly matching the
        // sample generator between the passes, which is error prone. The host side will issue a warning instead.
        return camera.computeRayPinhole(launchIndex, launchDim).dir;
    }
}

/**
 * Traces a shadow ray towards a light source.
 * @param[in] origin Ray origin for the shadow ray.
 * @param[in] dir Direction from shading point towards the light source (normalized).
 * @param[in] distance Distance to the light source.
 * @return True if light is visible, false otherwise.
 */
bool traceShadowRay(float3 origin, float3 dir, float distance)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.f;
    ray.TMax = distance;

    ShadowRayData rayData;
    rayData.visible = false; // Set to true by miss shader if ray is not terminated before
    TraceRay(
        gScene.rtAccel,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
        0xff /* instanceInclusionMask */,
        1 /* hitIdx */,
        rayTypeCount,
        1 /* missIdx */,
        ray,
        rayData
    );

    return rayData.visible;
}

/**
 * Traces a scatter ray based on ray parameters stored in the ray payload.
 * @param[in] rayData Describes the ray parameters. The struct is modified based on the result.
 */
void traceScatterRay(inout ScatterRayData rayData)
{
    RayDesc ray;
    ray.Origin = rayData.origin;
    ray.Direction = rayData.direction;
    ray.TMin = 0.f;
    ray.TMax = kRayTMax;

    uint rayFlags = 0; // TODO: Set cull mode from the app
    TraceRay(gScene.rtAccel, rayFlags, 0xff /* instanceInclusionMask */, 0 /* hitIdx */, rayTypeCount, 0 /* missIdx */, ray, rayData);
}

/**
 * Evaluates the direct illumination from analytic lights.
 * This function samples Falcor's light list uniformly with one shadow ray.
 * @param[in] sd Shading data.
 * @param[in] mi Material instance.
 * @param[in,out] sg SampleGenerator object.
 * @return Outgoing radiance in view direction.
 */
float3 evalDirectAnalytic(const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg)
{
    const uint lightCount = gScene.getLightCount();
    if (lightCount == 0)
        return float3(0.f);

    // Pick one of the analytic light sources randomly with equal probability.
    const uint lightIndex = min(uint(sampleNext1D(sg) * lightCount), lightCount - 1);
    float invPdf = lightCount; // Light selection pdf = 1.0 / lightCount.

    // Sample local light source.
    AnalyticLightSample ls;
    if (!sampleLight(sd.posW, gScene.getLight(lightIndex), sg, ls))
        return float3(0.f);

    // Reject sample if not in the hemisphere of a BSDF lobe.
    const uint lobeTypes = mi.getLobeTypes(sd);
    const bool hasReflection = lobeTypes & uint(LobeType::Reflection);
    const bool hasTransmission = lobeTypes & uint(LobeType::Transmission);
    float NdotL = dot(sd.getOrientedFaceNormal(), ls.dir);
    if ((NdotL <= kMinCosTheta && !hasTransmission) || (NdotL >= -kMinCosTheta && !hasReflection))
        return float3(0.f);

    // Get origin with offset applied in direction of the geometry normal to avoid self-intersection.
    const float3 origin = computeRayOrigin(sd.posW, dot(sd.faceN, ls.dir) >= 0.f ? sd.faceN : -sd.faceN);

    // Test visibility by tracing a shadow ray.
    bool V = traceShadowRay(origin, ls.dir, ls.distance);
    if (!V)
        return float3(0.f);

    // Evaluate contribution.
    return mi.eval(sd, ls.dir, sg) * ls.Li * invPdf;
}

bool canSampleEmissiveLights(const ShadingData sd, const IMaterialInstance mi)
{
    return kUseEmissiveLights && gScene.lightCollection.getActiveTriangleCount() > 0 &&
        (mi.getLobeTypes(sd) & uint(LobeType::NonDelta)) != 0;
}

// 两种策略各取一个样本，输入为立体角 PDF。
float powerHeuristic(float pdf, float otherPdf)
{
    float ratio = otherPdf / pdf;
    return 1.f / (1.f + ratio * ratio);
}

bool isMaskedLightSample(const TriangleLightSample light)
{
    const uint materialID = gScene.lightCollection.getTriangle(light.triangleIndex).materialID;
    if (gScene.materials.getMaterialHeader(materialID).getAlphaMode() != AlphaMode::Mask)
        return false;

    const uint2 meshPrimitive = gScene.lightCollection.getSceneMeshPrimID(light.triangleIndex);
    GeometryInstanceID instanceID;
    instanceID.index = meshPrimitive.x;
    const float3 barycentrics = float3(1.f - light.uv.x - light.uv.y, light.uv);
    const VertexData vertex = gScene.getVertexData(instanceID, meshPrimitive.y, barycentrics);
    return gScene.materials.alphaTest(vertex, materialID, 0.f);
}

float3 evalDirectEmissive(const ShadingData sd, const IMaterialInstance mi, inout SampleGenerator sg, const GuideContext guide)
{
    EmissiveUniformSampler sampler = {};
    TriangleLightSample light;
    // 均匀选择一张发光三角形，再在其表面均匀采点。采样器返回立体角 PDF。
    if (!sampler.sampleLight(sd.posW, sd.getOrientedFaceNormal(), false, sg, light))
        return float3(0.f);
    if (!(light.pdf > 0.f && light.pdf < FLT_MAX) || !any(light.Le > 0.f))
        return float3(0.f);
    if (isMaskedLightSample(light))
        return float3(0.f);

    const float3 scatter = mi.eval(sd, light.dir, sg);
    if (!any(scatter > 0.f))
        return float3(0.f);

    // 阴影射线使用偏移后的端点，BSDF 与 PDF 使用实际着色点和采样方向。
    const float3 origin = sd.computeRayOrigin(dot(sd.getOrientedFaceNormal(), light.dir) >= 0.f);
    const float3 segment = computeRayOrigin(light.posW, light.normalW) - origin;
    if (!(dot(segment, light.dir) > 0.f))
        return float3(0.f);
    const float distance = length(segment);
    if (!traceShadowRay(origin, segment / distance, distance))
        return float3(0.f);

    // 参考余弦采样的透射半球可能返回带符号的值，MIS 使用概率密度的绝对值。
    float scatterPdf = abs(mi.evalPdf(sd, light.dir, kUseImportanceSampling));
#if USE_RADIANCE_CACHE
    if (guide.mix > 0.f)
    {
        float q;
        const uint bin = getGuideBin(guide, light.dir, q);
        scatterPdf = mixScatterPdf(guide, bin, q, scatterPdf);
    }
#endif
    return scatter * light.Le * (powerHeuristic(light.pdf, scatterPdf) / light.pdf);
}

float evalEmissiveHitWeight(const HitInfo hit, const ShadingData sd, float3 scatterPositionW, float scatterPdf)
{
    if (!(scatterPdf > 0.f) || hit.getType() != HitType::Triangle)
        return 1.f;

    const TriangleHit triangle = hit.getTriangleHit();
    const uint triangleIndex = gScene.lightCollection.getTriangleIndex(triangle.instanceID, triangle.primitiveIndex);
    if (triangleIndex == LightCollection::kInvalidIndex)
        return 1.f;
    if (gScene.lightCollection.getActiveTriangleIndex(triangleIndex) == LightCollection::kInvalidIndex)
        return 1.f;

    TriangleLightHit lightHit;
    lightHit.triangleIndex = triangleIndex;
    lightHit.posW = sd.posW;
    lightHit.normalW = gScene.lightCollection.getTriangle(triangleIndex).normal;
    EmissiveUniformSampler sampler = {};
    // 均匀采样器的选灯概率与着色点的法线无关。
    const float lightPdf = sampler.evalPdf(scatterPositionW, float3(0.f), false, lightHit);
    return powerHeuristic(scatterPdf, lightPdf);
}

/**
 * Generate a new scatter ray or terminate.
 * @param[in] sd Shading data.
 * @param[in] mi Material instance.
 * @param[in] isCurveHit True if on curve hit.
 * @param[in] rayOrigin Ray origin for the new ray.
 * @param[in] sampledEmissive 当前散射点是否使用了发光网格采样策略。
 * @param[in,out] rayData Ray payload.
 * @return True if the path continues.
 */
bool generateScatterRay(
    const ShadingData sd, const IMaterialInstance mi, bool isCurveHit, float3 rayOrigin, bool sampledEmissive, inout ScatterRayData rayData
)
{
    // Sample material.
    BSDFSample bsdfSample = {};
    if (mi.sample(sd, rayData.sg, bsdfSample, kUseImportanceSampling))
    {
        rayData.origin = rayOrigin;
        if (!isCurveHit && bsdfSample.isLobe(LobeType::Transmission))
        {
            rayData.origin = sd.computeRayOrigin(false);
        }
        rayData.direction = bsdfSample.wo;
        rayData.scatterPositionW = sd.posW;
        rayData.scatterPdf = sampledEmissive && !bsdfSample.isLobe(LobeType::Delta) ? bsdfSample.pdf : 0.f;
        rayData.thp *= bsdfSample.weight;
        return any(rayData.thp > 0.f);
    }

    return false;
}

void traceScatterPath(inout ScatterRayData rayData)
{
    for (uint depth = 0; depth <= kMaxBounces && !rayData.terminated; depth++)
        traceScatterRay(rayData);
}

void addHitEmission(inout ScatterRayData rayData, const float3 contribution)
{
#if USE_RADIANCE_CACHE
    if (rayData.splitFirstHitEmission && rayData.pathLength == 0)
    {
        rayData.firstHitEmission += contribution;
        return;
    }
#endif
    rayData.radiance += contribution;
}

#if USE_RADIANCE_CACHE
float3 traceCachedScatter(
    const ShadingData sd, const IMaterialInstance mi, const float3 rayOrigin, const bool sampledEmissive,
    const GuideContext guide, inout SampleGenerator sg
)
{
    // 即使 BSDF 返回空事件，该次抽样仍包含代理积分。
    float3 estimate = guide.cvStrength * guide.cvIntegral;
    BSDFSample sample = {};
    if (guide.mix > 0.f && sampleNext1D(sg) < guide.mix)
        sample.wo = sampleGuide(guide, sg);
    else if (!mi.sample(sd, sg, sample, kUseImportanceSampling))
        return estimate;

    float q;
    const uint bin = getGuideBin(guide, sample.wo, q);
    float pdf = abs(sample.pdf);
    if (guide.mix > 0.f)
    {
        const float bsdfPdf = abs(mi.evalPdf(sd, sample.wo, kUseImportanceSampling));
        pdf = mixScatterPdf(guide, bin, q, bsdfPdf);
    }
    if (!(pdf > 0.f && isfinite(pdf)))
        return estimate;

    const float relativeWeight = q / pdf;
    if (guide.cvStrength > 0.f && bin != kInvalidCacheIndex)
    {
        const float3 proxy = guide.albedo * gCacheBins[bin].radiance * (1.f - guide.minCosine * guide.minCosine);
        estimate -= guide.cvStrength * proxy * relativeWeight;
    }

    const float3 weight = guide.mix > 0.f ? mi.eval(sd, sample.wo, sg) / pdf : sample.weight;
    if (!any(weight > 0.f))
        return estimate;

    // 首段吞吐量留在 raygen。后缀以单位吞吐量追踪以直接取得入射辐亮度。
    ScatterRayData rayData = ScatterRayData(sg);
    rayData.origin = rayOrigin;
    rayData.direction = sample.wo;
    rayData.scatterPositionW = sd.posW;
    rayData.scatterPdf = sampledEmissive ? pdf : 0.f;
    rayData.splitFirstHitEmission = true;
    traceScatterPath(rayData);
    if (bin != kInvalidCacheIndex)
        trainGuide(guide, bin, rayData.radiance, relativeWeight);

    return estimate + weight * (rayData.firstHitEmission + rayData.radiance);
}
#endif

/**
 * Process a hit.
 * Loads the shading data, samples analytic lights and samples a new scatter ray.
 * Terminates the path if maximum number of bounces is reached.
 * @param[in] hit Hit info.
 * @param[in,out] rayData Ray payload.
 *
 */
void handleHit(const HitInfo hit, inout ScatterRayData rayData)
{
    const bool isCurveHit = hit.getType() == HitType::Curve;

    // Load shading data.
    ShadingData sd = loadShadingData(hit, rayData.origin, rayData.direction);

    // Create material instance.
    let lod = ExplicitLodTextureSampler(0.f);
    let mi = gScene.materials.getMaterialInstance(sd, lod);

    // Add emitted light.
    if (kUseEmissiveLights && (kComputeDirect || rayData.pathLength > 0))
    {
        float3 emission = mi.getProperties(sd).emission;
        if (any(emission > 0.f))
        {
            if (gScene.materials.hasLightProfile(sd.materialID))
                emission *= gScene.materials.evalLightProfile(dot(sd.faceN, sd.V));
            const float weight = evalEmissiveHitWeight(hit, sd, rayData.scatterPositionW, rayData.scatterPdf);
            addHitEmission(rayData, rayData.thp * emission * weight);
        }
    }

    // Check whether to terminate based on max depth.
    if (rayData.pathLength >= kMaxBounces)
    {
        rayData.terminated = true;
        return;
    }

    // Compute ray origin for new rays spawned from the hit.
    float3 rayOrigin;
    if (isCurveHit)
    {
        // For curves, we set the new origin at the sphere center.
        rayOrigin = sd.posW - sd.curveRadius * sd.frame.N;
    }
    else
    {
        rayOrigin = sd.computeRayOrigin();
    }

    // Add contribution of direct light from analytic lights.
    if (kUseAnalyticLights)
    {
        float3 Lr = evalDirectAnalytic(sd, mi, rayData.sg);
        rayData.radiance += rayData.thp * Lr;
    }

    const bool sampledEmissive = canSampleEmissiveLights(sd, mi);
    if (sampledEmissive)
    {
        const GuideContext guide = {};
        rayData.radiance += rayData.thp * evalDirectEmissive(sd, mi, rayData.sg, guide);
    }

    // Generate scatter ray for the next path segment.
    // The raygen shader will continue the path based on the returned payload.
    if (!generateScatterRay(sd, mi, isCurveHit, rayOrigin, sampledEmissive, rayData))
    {
        rayData.terminated = true;
        return;
    }

    rayData.pathLength++;
}

// 读取主命中后逐段追踪，环境光在散射射线 miss 时求值。
float3 tracePath(const uint2 pixel, const uint2 frameDim)
{
    float3 outColor = float3(0.f);

    const float3 primaryRayOrigin = gScene.camera.getPosition();
    const float3 primaryRayDir = getPrimaryRayDir(pixel, frameDim, gScene.camera);

    const HitInfo hit = HitInfo(gVBuffer[pixel]);
    if (hit.isValid())
    {
        // Pixel represents a valid primary hit. Compute its contribution.

        const bool isCurveHit = hit.getType() == HitType::Curve;

        // Load shading data.
        ShadingData sd = loadShadingData(hit, primaryRayOrigin, primaryRayDir);

        // Create material instance at shading point.
        let lod = ExplicitLodTextureSampler(0.f);
        let mi = gScene.materials.getMaterialInstance(sd, lod);

        // Create sample generator.
        SampleGenerator sg = SampleGenerator(pixel, gFrameCount);

        // Advance the generator to the first available dimension.
        // TODO: This is potentially expensive. We may want to store/restore the state from memory if it becomes a problem.
        for (uint i = 0; i < gPRNGDimension; i++)
            sampleNext1D(sg);

        // Compute ray origin for new rays spawned from the G-buffer.
        float3 rayOrigin;
        if (isCurveHit)
        {
            // For curves, we set the new origin at the sphere center.
            rayOrigin = sd.posW - sd.curveRadius * sd.frame.N;
        }
        else
        {
            rayOrigin = sd.computeRayOrigin();
        }

        GuideContext guide = {};
#if USE_RADIANCE_CACHE
        guide = getGuideContext(hit, sd, mi);
#endif
        const bool sampledEmissive = kComputeDirect && canSampleEmissiveLights(sd, mi);
        if (kComputeDirect)
        {
            // Always output directly emitted light, independent of whether emissive materials are treated as light sources or not.
            outColor += mi.getProperties(sd).emission;

            // 解析光源和发光网格各取一个直接光样本。
            outColor += kUseAnalyticLights ? evalDirectAnalytic(sd, mi, sg) : float3(0.f);
            if (sampledEmissive)
                outColor += evalDirectEmissive(sd, mi, sg, guide);
        }

#if USE_RADIANCE_CACHE
        if (guide.cellIndex != kInvalidCacheIndex)
            return outColor + traceCachedScatter(sd, mi, rayOrigin, sampledEmissive, guide, sg);
#endif

        // Prepare ray payload.
        ScatterRayData rayData = ScatterRayData(sg);

        // Generate scatter ray.
        if (!generateScatterRay(sd, mi, isCurveHit, rayOrigin, sampledEmissive, rayData))
            rayData.terminated = true;

        traceScatterPath(rayData);

        // Store contribution from scatter ray.
        outColor += rayData.radiance;
    }
    else
    {
        // Background pixel.
        outColor = kUseEnvBackground ? gScene.envMap.eval(primaryRayDir) : kDefaultBackgroundColor;
    }

    return outColor;
}

//
// Shader entry points for miss shaders.
//

[shader("miss")]
void scatterMiss(inout ScatterRayData rayData)
{
    // Ray missed the scene. Mark the ray as terminated.
    rayData.terminated = true;

    // Add contribution from distant light (env map) in this direction.
    if (kUseEnvLight && (kComputeDirect || rayData.pathLength > 0))
    {
        float3 Le = gScene.envMap.eval(WorldRayDirection());
        addHitEmission(rayData, rayData.thp * Le);
    }
}

[shader("miss")]
void shadowMiss(inout ShadowRayData rayData)
{
    // The miss shader is executed if the ray misses all geometry. Mark as visible.
    rayData.visible = true;
}

//
// Shader entry points for TriangleMesh hit groups.
//

[shader("anyhit")]
void scatterTriangleMeshAnyHit(inout ScatterRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f))
        IgnoreHit();
}

[shader("closesthit")]
void scatterTriangleMeshClosestHit(inout ScatterRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit triangleHit;
    triangleHit.instanceID = getGeometryInstanceID();
    triangleHit.primitiveIndex = PrimitiveIndex();
    triangleHit.barycentrics = attribs.barycentrics;
    handleHit(HitInfo(triangleHit), rayData);
}

[shader("anyhit")]
void shadowTriangleMeshAnyHit(inout ShadowRayData rayData, BuiltInTriangleIntersectionAttributes attribs)
{
    // Alpha test for non-opaque geometry.
    GeometryInstanceID instanceID = getGeometryInstanceID();
    VertexData v = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(v, materialID, 0.f))
        IgnoreHit();
}

//
// Shader entry points for DisplacedTriangleMesh hit groups.
//

[shader("intersection")]
void displacedTriangleMeshIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    DisplacedTriangleMeshIntersector::Attribs attribs;
    float t;
    if (DisplacedTriangleMeshIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attribs, t))
    {
        ReportHit(t, 0, attribs);
    }
}

[shader("closesthit")]
void scatterDisplacedTriangleMeshClosestHit(inout ScatterRayData rayData, DisplacedTriangleMeshIntersector::Attribs attribs)
{
    DisplacedTriangleHit displacedTriangleHit;
    displacedTriangleHit.instanceID = getGeometryInstanceID();
    displacedTriangleHit.primitiveIndex = PrimitiveIndex();
    displacedTriangleHit.barycentrics = attribs.barycentrics;
    displacedTriangleHit.displacement = attribs.displacement;
    handleHit(HitInfo(displacedTriangleHit), rayData);
}

//
// Shader entry points for Curve hit groups.
//

[shader("intersection")]
void curveIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    CurveIntersector::Attribs attribs;
    float t;
    if (CurveIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attribs, t))
    {
        ReportHit(t, 0, attribs);
    }
}

[shader("closesthit")]
void scatterCurveClosestHit(inout ScatterRayData rayData, CurveIntersector::Attribs attribs)
{
    CurveHit curveHit;
    curveHit.instanceID = getGeometryInstanceID();
    curveHit.primitiveIndex = PrimitiveIndex();
    curveHit.barycentrics = attribs.barycentrics;
    handleHit(HitInfo(curveHit), rayData);
}

//
// Shader entry points for SDFGrid hit groups.
//

[shader("intersection")]
void sdfGridIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    SDFGridHitData sdfGridHitData;
    float t;
    if (SDFGridIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), sdfGridHitData, t))
    {
        ReportHit(t, 0, sdfGridHitData);
    }
}

[shader("closesthit")]
void scatterSdfGridClosestHit(inout ScatterRayData rayData, SDFGridHitData sdfGridHitData)
{
    SDFGridHit sdfGridHit;
    sdfGridHit.instanceID = getGeometryInstanceID();
    sdfGridHit.hitData = sdfGridHitData;
    handleHit(HitInfo(sdfGridHit), rayData);
}

//
// Shader entry point for ray generation shader.
//

[shader("raygeneration")]
void rayGen()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 frameDim = DispatchRaysDimensions().xy;

    float3 color = tracePath(pixel, frameDim);

    gOutputColor[pixel] = float4(color, 1.f);
}
