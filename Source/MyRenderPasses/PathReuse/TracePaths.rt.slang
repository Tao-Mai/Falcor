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

#include "Scene/SceneDefines.slangh"
#include "Utils/Math/MathConstants.slangh"
#include "PathReuseParams.slangh"

import Scene.Raytracing;
import Scene.Intersection;
import Utils.Geometry.GeometryHelpers;
import Utils.Math.Ray;
import Utils.Sampling.SampleGenerator;
import Rendering.Lights.EmissiveLightSamplerInterface;
import Rendering.Lights.EmissiveUniformSampler;
import Rendering.Lights.LightHelpers;
import MyRenderPasses.Shared.EmissiveLightHelpers;
import PathReuseSample;

ConstantBuffer<TracePathsParams> gTraceParams;

Texture2D<PackedHitInfo> gVBuffer;
Texture2D<float4> gViewW;
RWStructuredBuffer<PathReuseSample> gPathSamples;

static const uint kMaxBounces = MAX_BOUNCES;
static const bool kComputeDirect = COMPUTE_DIRECT;
static const bool kUseImportanceSampling = USE_IMPORTANCE_SAMPLING;
static const bool kUseAnalyticLights = USE_ANALYTIC_LIGHTS;
static const bool kUseEmissiveLights = USE_EMISSIVE_LIGHTS;
static const bool kUseEnvLight = USE_ENV_LIGHT;
static const bool kUseEnvBackground = USE_ENV_BACKGROUND;

// x 是主命中，y 是重连顶点，z 是 BSDF 后缀的首个命中。
[shader("raygeneration")]
void rayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 frameDim = DispatchRaysDimensions().xy;
    PixelPathTracer tracer = {};
    tracer.trace(pixel, frameDim);
    gPathSamples[pixel.y * frameDim.x + pixel.x] = tracer.path;
}

struct PixelPathTracer
{
    PathReuseSample path;
    SampleGenerator sg;

    [mutating]
    void trace(uint2 pixel, uint2 frameDim)
    {
        const Ray cameraRay = getCameraRay(pixel, frameDim);
        const HitInfo primaryHit = HitInfo(gVBuffer[pixel]);
        if (!primaryHit.isValid())
        {
            path.localRadiance = kUseEnvBackground ? gScene.envMap.eval(cameraRay.dir) : float3(0.f);
            return;
        }

        sg = SampleGenerator(pixel, gTraceParams.frameIndex);
        for (uint dimension = 0; dimension < gTraceParams.prngDimension; ++dimension)
            sampleNext1D(sg);

        // x 的自发光和 x → 灯留在本像素。
        const TraceVertex primary = TraceVertex(primaryHit, cameraRay);
        let primaryMaterial = primary.getMaterial();
        path.primary = primary.reuseVertex();
        const bool primaryNee = kComputeDirect && primary.canSampleEmissive(primaryMaterial);
        if (kComputeDirect)
        {
            path.localRadiance = primaryMaterial.getProperties(primary.shading).emission;
            primary.addDirectLighting(primaryMaterial, primaryNee, float3(1.f), sg, path.localRadiance);
        }

        // BSDF 生成 x → y。若这条射线直接到达光源，仍计入 x 的直接光。直接光不复用
        BSDFSample first = {};
        if (!primary.sampleBsdf(primaryMaterial, sg, first))
            return;
        const ScatterSegment segment = primary.scatter(first, primaryNee);
        const HitInfo secondaryHit = traceScatterRay(segment.ray);
        if (!secondaryHit.isValid())
        {
            const float3 environment = kUseEnvLight && kComputeDirect ? gScene.envMap.eval(segment.ray.dir) : float3(0.f);
            path.localRadiance += first.weight * environment;
            return;
        }

        const TraceVertex secondary = TraceVertex(secondaryHit, segment.ray);
        let secondaryMaterial = secondary.getMaterial();
        if (kComputeDirect && kUseEmissiveLights)
            path.localRadiance += secondary.evaluateEmission(secondaryMaterial, segment).weighted(first.weight, segment.bsdfPdfW);

        // y 提供 y → 灯和 y → z → … 两条后缀。
        if (kMaxBounces > 0)
            traceSuffixes(primary, first, secondary, secondaryMaterial);
    }

    [mutating]
    void traceSuffixes(
        const TraceVertex primary, const BSDFSample first, const TraceVertex secondary, const IMaterialInstance material
    )
    {
        path.secondary = primary.reuseConnection(secondary, first);

        // 解析光源无需MIS
        // 这里不复用，只复用发光网格的好了
        if (kUseAnalyticLights)
            path.localRadiance += first.weight * secondary.estimateAnalyticDirect(material, sg);

        const bool sampleNee = secondary.canSampleEmissive(material);
        if (sampleNee)
            traceNeeSuffix(secondary, material, first.weight);
        traceBsdfSuffix(secondary, material, first.weight, sampleNee);
    }

    [mutating]
    void traceNeeSuffix(const TraceVertex vertex, const IMaterialInstance material, float3 prefixThroughput)
    {
        EmissiveLightEvaluation evaluation;
        if (!vertex.sampleEmissiveLight(material, sg, evaluation))
            return;

        const TriangleLightSample light = evaluation.light;
        const float3 contribution = prefixThroughput * evaluation.scatter * light.Le / light.pdf;
        if (!path.secondary.supportsSuffixPdf(light.pdf) || !isReflectionDirection(vertex.shading, light.dir))
        {
            path.localRadiance += contribution * powerHeuristic(light.pdf, evaluation.bsdfPdfW);
            return;
        }

        path.secondary.nee = {light.dir, light.pdf, light.Le, evaluation.bsdfPdfW, contribution};
    }

    [mutating]
    void traceBsdfSuffix(
        const TraceVertex vertex, const IMaterialInstance material, float3 prefixThroughput, bool sampleNee
    )
    {
        BSDFSample bsdf = {};
        if (!vertex.sampleBsdf(material, sg, bsdf))
            return;

        const float3 throughput = prefixThroughput * bsdf.weight;
        const bool canReuse = path.secondary.supportsSuffixPdf(bsdf.pdf) &&
            bsdf.isLobe(LobeType::NonDeltaReflection) && isReflectionDirection(vertex.shading, bsdf.wo);
        if (!canReuse && !any(throughput > 0.f))
            return;

        const ScatterSegment segment = vertex.scatter(bsdf, sampleNee);
        const ContinuationRadiance lighting = traceContinuation(segment);
        if (!canReuse)
        {
            path.localRadiance += throughput * lighting.withLocalMis(segment.bsdfPdfW);
            return;
        }

        path.secondary.bsdf = {bsdf.wo, bsdf.pdf, lighting, throughput};
    }

    [mutating]
    ContinuationRadiance traceContinuation(ScatterSegment incoming)
    {
        ContinuationRadiance lighting = {};
        float3 throughput = float3(1.f);
        for (uint depth = 1; depth <= kMaxBounces; ++depth)
        {
            const HitInfo hit = traceScatterRay(incoming.ray);
            if (!hit.isValid())
            {
                const float3 environment = kUseEnvLight ? gScene.envMap.eval(incoming.ray.dir) : float3(0.f);
                lighting.continuation += throughput * environment;
                break;
            }

            const TraceVertex vertex = TraceVertex(hit, incoming.ray);
            let material = vertex.getMaterial();
            if (kUseEmissiveLights)
            {
                const EmissionSample emission = vertex.evaluateEmission(material, incoming);
                // z 的自发光权重由 b2 的入射方向决定，重连后再做本地 MIS。
                if (depth == 1)
                {
                    lighting.emission = emission.radiance;
                    lighting.lightPdfW = emission.lightPdfW;
                }
                else
                {
                    lighting.continuation += emission.weighted(throughput, incoming.bsdfPdfW);
                }
            }
            if (depth == kMaxBounces)
                break;

            const bool sampleNee = vertex.canSampleEmissive(material);
            vertex.addDirectLighting(material, sampleNee, throughput, sg, lighting.continuation);

            BSDFSample bsdf = {};
            if (!vertex.sampleBsdf(material, sg, bsdf))
                break;
            throughput *= bsdf.weight;
            if (!any(throughput > 0.f))
                break;

            incoming = vertex.scatter(bsdf, sampleNee);
        }
        return lighting;
    }
};

struct ShadowPayload
{
    bool visible;
};

struct ScatterPayload
{
    PackedHitInfo hit;
};

ShadingData loadShadingData(const HitInfo hit, const Ray ray)
{
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_TRIANGLE_MESH)
    if (hit.getType() == HitType::Triangle)
        return loadTriangleShadingData(hit.getTriangleHit(), -ray.dir);
#endif

    VertexData vertex = {};
    uint materialID = 0;
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_DISPLACED_TRIANGLE_MESH)
    if (hit.getType() == HitType::DisplacedTriangle)
    {
        const DisplacedTriangleHit displacedHit = hit.getDisplacedTriangleHit();
        vertex = gScene.getVertexData(displacedHit, -ray.dir);
        materialID = gScene.getMaterialID(displacedHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_CURVE)
    if (hit.getType() == HitType::Curve)
    {
        const CurveHit curveHit = hit.getCurveHit();
        vertex = gScene.getVertexDataFromCurve(curveHit);
        materialID = gScene.getMaterialID(curveHit.instanceID);
    }
#endif
#if SCENE_HAS_GEOMETRY_TYPE(GEOMETRY_TYPE_SDF_GRID)
    if (hit.getType() == HitType::SDFGrid)
    {
        const SDFGridHit sdfHit = hit.getSDFGridHit();
        vertex = gScene.getVertexDataFromSDFGrid(sdfHit, ray.origin, ray.dir);
        materialID = gScene.getMaterialID(sdfHit.instanceID);
    }
#endif
    return gScene.materials.prepareShadingData(vertex, materialID, -ray.dir);
}

Ray getCameraRay(uint2 pixel, uint2 frameDim)
{
    const float3 origin = gScene.camera.getPosition();
    if (is_valid_gViewW != 0)
        return Ray(origin, -gViewW[pixel].xyz);
    return Ray(origin, gScene.camera.computeRayPinhole(pixel, frameDim).dir);
}

bool traceShadowRay(const Ray ray)
{
    ShadowPayload payload = {};
    TraceRayScene(RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xff, kPathReuseShadowRay, kPathReuseShadowRay, ray.toRayDesc(), payload);
    return payload.visible;
}

HitInfo traceScatterRay(const Ray ray)
{
    ScatterPayload payload = {};
    TraceRayScene(RAY_FLAG_NONE, 0xff, kPathReuseScatterRay, kPathReuseScatterRay, ray.toRayDesc(), payload);
    return HitInfo(payload.hit);
}

struct ScatterSegment
{
    Ray ray;
    float3 scatterPositionW; // 灯光 PDF 使用偏移前的着色点。
    float bsdfPdfW; // 仅记录参与灯光 MIS 的 BSDF 密度。
};

struct EmissionSample
{
    float3 radiance;
    float lightPdfW;

    float3 weighted(float3 throughput, float bsdfPdfW)
    {
        return throughput * radiance * powerHeuristic(bsdfPdfW, lightPdfW);
    }
};

struct EmissiveLightEvaluation
{
    TriangleLightSample light;
    float3 scatter;
    float bsdfPdfW;

    float3 localEstimate()
    {
        return scatter * light.Le * (powerHeuristic(light.pdf, bsdfPdfW) / light.pdf);
    }
};

struct TraceVertex
{
    HitInfo hit;
    ShadingData shading;

    __init(const HitInfo hit, const Ray incoming)
    {
        this.hit = hit;
        shading = loadShadingData(hit, incoming);
    }

    IMaterialInstance getMaterial()
    {
        return gScene.materials.getMaterialInstance(shading, ExplicitLodTextureSampler(0.f));
    }

    bool sampleBsdf(const IMaterialInstance material, inout SampleGenerator sg, out BSDFSample bsdf)
    {
        return material.sample(shading, sg, bsdf, kUseImportanceSampling);
    }

    ScatterSegment scatter(const BSDFSample bsdf, bool sampledEmissive)
    {
        const float3 origin = hit.getType() == HitType::Curve
            ? shading.posW - shading.curveRadius * shading.frame.N
            : shading.computeRayOrigin(!bsdf.isLobe(LobeType::Transmission));

        ScatterSegment segment;
        segment.ray = Ray(origin, bsdf.wo);
        segment.scatterPositionW = shading.posW;
        segment.bsdfPdfW = sampledEmissive && !bsdf.isLobe(LobeType::Delta) ? bsdf.pdf : 0.f;
        return segment;
    }

    PrimaryVertexSample reuseVertex()
    {
        if (kMaxBounces == 0 || hit.getType() != HitType::Triangle)
            return {};
        return {hit, shading.V};
    }

    SecondaryPathSample reuseConnection(const TraceVertex next, const BSDFSample bsdf)
    {
        if (hit.getType() != HitType::Triangle || next.hit.getType() != HitType::Triangle)
            return {};
        if (!bsdf.isLobe(LobeType::NonDeltaReflection) || !isReflectionDirection(shading, bsdf.wo))
            return {};

        const float3 connection = next.shading.posW - shading.posW;
        const float distanceSquared = dot(connection, connection);
        if (distanceSquared <= 0.f)
            return {};

        const float pdfA = bsdf.pdf * solidAngleToAreaJacobian(next.shading.faceN, bsdf.wo, distanceSquared);
        if (!(pdfA > 0.f && pdfA < FLT_MAX))
            return {};

        SecondaryPathSample sample = {};
        sample.hit = next.hit;
        sample.primaryToSecondaryPdfA = pdfA;
        return sample;
    }

    // 启用了发光网格且有发光网格且当前材质有非delta的散射分量
    bool canSampleEmissive(const IMaterialInstance material)
    {
        return kUseEmissiveLights && gScene.lightCollection.getActiveTriangleCount() > 0 && hasContinuousLobes(shading, material);
    }

    bool sampleEmissiveLight(const IMaterialInstance material, inout SampleGenerator sg, out EmissiveLightEvaluation result)
    {
        EmissiveUniformSampler sampler = {};
        TriangleLightSample light;
        if (!sampler.sampleLight(shading.posW, shading.getOrientedFaceNormal(), false, sg, light))
            return false;
        if (!(light.pdf > 0.f && light.pdf < FLT_MAX) || !any(light.Le > 0.f))
            return false;
        if (isMaskedLightSample(light))
            return false;

        const float3 origin = shading.computeRayOrigin(isReflectionDirection(shading, light.dir));
        const float3 segment = computeRayOrigin(light.posW, light.normalW) - origin;
        if (!(dot(segment, light.dir) > 0.f))
            return false;
        const float distance = length(segment);
        if (!traceShadowRay(Ray(origin, segment / distance, 0.f, distance)))
            return false;

        result.light = light;
        result.bsdfPdfW = evalBsdfPdfW(shading, material, light.dir, kUseImportanceSampling);
        result.scatter = material.eval(shading, light.dir, sg);
        return true;
    }

    float3 estimateEmissiveDirect(const IMaterialInstance material, inout SampleGenerator sg)
    {
        EmissiveLightEvaluation evaluation;
        if (!sampleEmissiveLight(material, sg, evaluation))
            return float3(0.f);
        return evaluation.localEstimate();
    }

    float3 estimateAnalyticDirect(const IMaterialInstance material, inout SampleGenerator sg)
    {
        const uint lightCount = gScene.getLightCount();
        if (lightCount == 0)
            return float3(0.f);

        const uint lightIndex = min(uint(sampleNext1D(sg) * lightCount), lightCount - 1);
        AnalyticLightSample light;
        if (!sampleLight(shading.posW, gScene.getLight(lightIndex), sg, light))
            return float3(0.f);

        const uint lobes = material.getLobeTypes(shading);
        const bool hasReflection = (lobes & uint(LobeType::Reflection)) != 0;
        const bool hasTransmission = (lobes & uint(LobeType::Transmission)) != 0;
        const float cosTheta = dot(shading.getOrientedFaceNormal(), light.dir);
        if ((cosTheta <= kMinCosTheta && !hasTransmission) || (cosTheta >= -kMinCosTheta && !hasReflection))
            return float3(0.f);

        const float3 origin = shading.computeRayOrigin(cosTheta >= 0.f);
        if (!traceShadowRay(Ray(origin, light.dir, 0.f, light.distance)))
            return float3(0.f);

        return material.eval(shading, light.dir, sg) * light.Li * lightCount;
    }

    void addDirectLighting(
        const IMaterialInstance material, bool sampleNee, float3 throughput, inout SampleGenerator sg, inout float3 radiance
    )
    {
        if (kUseAnalyticLights)
            radiance += throughput * estimateAnalyticDirect(material, sg);
        if (sampleNee)
            radiance += throughput * estimateEmissiveDirect(material, sg);
    }

    float emissiveHitPdfW(float3 scatterPositionW)
    {
        if (hit.getType() != HitType::Triangle)
            return 0.f;

        const TriangleHit triangle = hit.getTriangleHit();
        const uint triangleIndex = gScene.lightCollection.getTriangleIndex(triangle.instanceID, triangle.primitiveIndex);
        if (triangleIndex == LightCollection::kInvalidIndex)
            return 0.f;
        if (gScene.lightCollection.getActiveTriangleIndex(triangleIndex) == LightCollection::kInvalidIndex)
            return 0.f;

        TriangleLightHit lightHit;
        lightHit.triangleIndex = triangleIndex;
        lightHit.posW = shading.posW;
        lightHit.normalW = gScene.lightCollection.getTriangle(triangleIndex).normal;
        EmissiveUniformSampler sampler = {};
        return sampler.evalPdf(scatterPositionW, float3(0.f), false, lightHit);
    }

    EmissionSample evaluateEmission(const IMaterialInstance material, const ScatterSegment incoming)
    {
        EmissionSample emission = {};
        emission.radiance = material.getProperties(shading).emission;
        if (any(emission.radiance > 0.f) && gScene.materials.hasLightProfile(shading.materialID))
            emission.radiance *= gScene.materials.evalLightProfile(dot(shading.faceN, shading.V));
        if (incoming.bsdfPdfW > 0.f && any(emission.radiance > 0.f))
            emission.lightPdfW = emissiveHitPdfW(incoming.scatterPositionW);
        return emission;
    }
};

[shader("miss")]
void scatterMiss(inout ScatterPayload payload)
{
    payload.hit = {};
}

[shader("miss")]
void shadowMiss(inout ShadowPayload payload)
{
    payload.visible = true;
}

void alphaTestTriangle(BuiltInTriangleIntersectionAttributes attribs)
{
    const GeometryInstanceID instanceID = getGeometryInstanceID();
    const VertexData vertex = getVertexData(instanceID, PrimitiveIndex(), attribs);
    const uint materialID = gScene.getMaterialID(instanceID);
    if (gScene.materials.alphaTest(vertex, materialID, 0.f))
        IgnoreHit();
}

[shader("anyhit")]
void scatterTriangleMeshAnyHit(inout ScatterPayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    alphaTestTriangle(attribs);
}

[shader("anyhit")]
void shadowTriangleMeshAnyHit(inout ShadowPayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    alphaTestTriangle(attribs);
}

[shader("closesthit")]
void scatterTriangleMeshClosestHit(inout ScatterPayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    TriangleHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attribs.barycentrics;
    payload.hit = hit.pack();
}

[shader("intersection")]
void displacedTriangleMeshIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    DisplacedTriangleMeshIntersector::Attribs attribs;
    float t;
    if (DisplacedTriangleMeshIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attribs, t))
        ReportHit(t, 0, attribs);
}

[shader("closesthit")]
void scatterDisplacedTriangleMeshClosestHit(inout ScatterPayload payload, DisplacedTriangleMeshIntersector::Attribs attribs)
{
    DisplacedTriangleHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attribs.barycentrics;
    hit.displacement = attribs.displacement;
    payload.hit = hit.pack();
}

[shader("intersection")]
void curveIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    CurveIntersector::Attribs attribs;
    float t;
    if (CurveIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attribs, t))
        ReportHit(t, 0, attribs);
}

[shader("closesthit")]
void scatterCurveClosestHit(inout ScatterPayload payload, CurveIntersector::Attribs attribs)
{
    CurveHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.primitiveIndex = PrimitiveIndex();
    hit.barycentrics = attribs.barycentrics;
    payload.hit = hit.pack();
}

[shader("intersection")]
void sdfGridIntersection()
{
    const Ray ray = Ray(WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent());
    SDFGridHitData attribs;
    float t;
    if (SDFGridIntersector::intersect(ray, getGeometryInstanceID(), PrimitiveIndex(), attribs, t))
        ReportHit(t, 0, attribs);
}

[shader("closesthit")]
void scatterSdfGridClosestHit(inout ScatterPayload payload, SDFGridHitData attribs)
{
    SDFGridHit hit;
    hit.instanceID = getGeometryInstanceID();
    hit.hitData = attribs;
    payload.hit = hit.pack();
}
