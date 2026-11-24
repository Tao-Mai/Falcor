import PhotonMap;
import SurfaceTracing;
import MyRenderPasses.Shared.EmissiveLightHelpers;
import Rendering.Lights.EnvMapSampler;
import Utils.Sampling.SampleGenerator;

cbuffer CB
{
    PhotonGrid gGrid;
    uint gPhotonPaths;
    uint gPhotonBounces;
    uint gPhotonSeed;
    float3 gSceneCenter;
    float gSceneRadius;
    uint gUseEmissiveLights;
    uint gUseAnalyticLights;
    uint gUseEnvLight;
}

EnvMapSampler gEnvMapSampler;
RWStructuredBuffer<uint> gCellCounts;
RWStructuredBuffer<Photon> gPhotons;

bool emitPhoton(inout SampleGenerator sg, out Ray ray, out float3 flux)
{
    ray = Ray(float3(0.f), float3(0.f));
    flux = float3(0.f);

    // 光源选择属于本通道的策略，公共采样接口只处理已经选定的光源。
    const uint triangleCount = gUseEmissiveLights != 0u ? gScene.lightCollection.getActiveTriangleCount() : 0u;
    const uint analyticCount = gUseAnalyticLights != 0u ? gScene.getLightCount() : 0u;
    const uint categoryCount = uint(triangleCount > 0u) + uint(analyticCount > 0u) + uint(gUseEnvLight != 0u);
    if (categoryCount == 0u)
        return false;

    // 均匀选类别
    uint categoryIndex = categoryCount > 1u ? min(uint(sampleNext1D(sg) * categoryCount), categoryCount - 1u) : 0u;
    float selectionPdf = 1.f / float(categoryCount);
    LightEmissionSample emission;
    
    bool valid = false;
    if (triangleCount > 0u && categoryIndex == 0u)
    {
        const uint index = min(uint(sampleNext1D(sg) * triangleCount), triangleCount - 1u);
        const uint triangleIndex = gScene.lightCollection.activeTriangles[index];
        valid = sampleEmissiveLightEmission(triangleIndex, sg, emission);
        selectionPdf /= float(triangleCount);
    }
    else
    {
        if (triangleCount > 0u)
            --categoryIndex;
        if (analyticCount > 0u && categoryIndex == 0u)
        {
            const uint lightIndex = min(uint(sampleNext1D(sg) * analyticCount), analyticCount - 1u);
            valid = sampleLightEmission(gScene.getLight(lightIndex), gSceneCenter, gSceneRadius, sg, emission);
            selectionPdf /= float(analyticCount);
        }
        else if (gUseEnvLight != 0u)
            valid = sampleEnvMapEmission(gEnvMapSampler, gSceneCenter, gSceneRadius, sg, emission);
    }
    if (!valid)
        return false;

    ray = emission.ray;
    flux = emission.weight / (selectionPdf * float(gPhotonPaths));
    return all(isfinite(flux));
}

void storePhoton(const ShadingData sd, float3 directionToLight, float3 flux)
{
    const uint cell = gGrid.index(gGrid.cell(sd.posW));

    // 每个格子都有光子存储数上限，不过现在这种数据结构效率有点低。
    uint slot;
    InterlockedAdd(gCellCounts[cell], 1u, slot);
    if (slot >= kPhotonCellCapacity)
        return;

    Photon photon;
    photon.position = sd.posW;
    photon.normal = sd.getOrientedFaceNormal();
    photon.directionToLight = directionToLight;
    photon.flux = flux;
    gPhotons[cell * kPhotonCellCapacity + slot] = photon;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gPhotonPaths)
        return;
    SampleGenerator sg = SampleGenerator(uint2(tid.x, 0x3c6ef372u), gPhotonSeed);
    
    Ray ray;
    float3 flux;
    if (!emitPhoton(sg, ray, flux))
        return;

    SceneRayQuery<1> query;
    for (uint depth = 0u; depth <= gPhotonBounces; ++depth)
    {
        float hitT;
        const HitInfo hit = query.traceRay(ray, hitT);
        if (!hit.isValid())
            break;
        
        const ShadingData sd = loadSurface(hit, ray);
        let mi = gScene.materials.getMaterialInstance(sd, ExplicitLodTextureSampler(0.f));
        const uint lobes = mi.getLobeTypes(sd);

        // 只收集已经至少散射一次并到达漫反射表面的光子。
        if (depth > 0u && (lobes & uint(LobeType::DiffuseReflection)) != 0u && hit.getType() != HitType::Curve)
            storePhoton(sd, -ray.dir, flux);
        
        if (depth == gPhotonBounces)
            break;

        BSDFSample sample = {};
        if (!mi.sample(sd, sg, sample) || !any(sample.weight > 0.f))
            break;
        
        const float survival = clamp(max(sample.weight.x, max(sample.weight.y, sample.weight.z)), 0.05f, 0.95f);
        if (sampleNext1D(sg) >= survival)
            break;
        
        flux *= sample.weight / survival;
        if (!any(flux > 0.f) || !all(isfinite(flux)))
            break;
        
        ray = Ray(scatterOrigin(hit, sd, sample.wo), sample.wo);
    }
}
