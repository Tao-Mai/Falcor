#include "Utils/Math/MathConstants.slangh"

import Scene.Scene;
import Scene.Lights.LightCollection;
import Scene.Lights.LightData;
import Scene.Material.MaterialTypes;
import Rendering.Lights.EmissiveLightSamplerInterface;
import Rendering.Lights.EnvMapSampler;
import Utils.Geometry.GeometryHelpers;
import Utils.Math.MathHelpers;
import Utils.Math.Ray;
import Utils.Math.ShadingFrame;
import Utils.Sampling.SampleGeneratorInterface;

// 检查有效发光三角形上的采样点。返回 true 表示该点被 alpha mask 裁掉。
bool isMaskedEmitter(uint triangleIndex, uint materialID, float3 barycentrics)
{
    if (gScene.materials.getMaterialHeader(materialID).getAlphaMode() != AlphaMode::Mask)
        return false;

    const uint2 meshPrimitive = gScene.lightCollection.getSceneMeshPrimID(triangleIndex);
    GeometryInstanceID instanceID;
    instanceID.index = meshPrimitive.x;
    const VertexData vertex = gScene.getVertexData(instanceID, meshPrimitive.y, barycentrics);
    return gScene.materials.alphaTest(vertex, materialID, 0.f);
}

// TriangleLightSample.uv 保存重心坐标的后两维。
bool isMaskedLightSample(const TriangleLightSample light)
{
    const uint materialID = gScene.lightCollection.getTriangle(light.triangleIndex).materialID;
    const float3 barycentrics = float3(1.f - light.uv.x - light.uv.y, light.uv);
    return isMaskedEmitter(light.triangleIndex, materialID, barycentrics);
}

// 光端采样结果。与官方 AnalyticLightSample 类似，weight 已除条件采样 PDF。
// 选择光源的离散概率由调用方处理。
struct LightEmissionSample
{
    Ray ray = Ray(float3(0.f), float3(0.f));
    float3 posW = float3(0.f); // 面光源为偏移前的采样点，无限远光源为投影圆盘上的点。
    float3 normalW = float3(0.f);
    float positionPdf = 0.f; // 条件面积密度。deltaPosition 为 true 时表示离散质量 1。
    float directionPdf = 0.f; // 条件立体角密度。deltaDirection 为 true 时表示离散质量 1。
    bool deltaPosition = false;
    bool deltaDirection = false;
    float3 weight = float3(0.f); // 已除位置和方向 PDF 的 RGB 通量，所有光源类型均为同一含义。
};

// 对已经选定的发光三角形采样灯面点和余弦半球方向。
// 调用前应在 CPU 端请求 LightCollection，失败样本计零贡献。
bool sampleEmissiveLightEmission<S : ISampleGenerator>(uint triangleIndex, inout S sg, out LightEmissionSample result)
{
    result = {};
    const EmissiveTriangle light = gScene.lightCollection.getTriangle(triangleIndex);
    const float3 barycentrics = sample_triangle(sampleNext2D(sg));
    if (!(light.area > 0.f) || isMaskedEmitter(triangleIndex, light.materialID, barycentrics))
        return false;

    bool frameValid;
    const ShadingFrame frame = ShadingFrame::createSafe(light.normal, float4(0.f, 0.f, 0.f, 1.f), frameValid);
    float directionPdf;
    const float3 localDirection = sample_cosine_hemisphere_polar(sampleNext2D(sg), directionPdf);
    const float3 direction = frame.fromLocal(localDirection);
    float3 emitted = gScene.materials.evalEmissive(light.materialID, light.getTexCoord(barycentrics));
    if (gScene.materials.hasLightProfile(light.materialID))
        emitted *= gScene.materials.evalLightProfile(max(0.f, dot(light.normal, direction)));

    result.posW = light.getPosition(barycentrics);
    result.normalW = light.normal;
    result.ray = Ray(computeRayOrigin(result.posW, light.normal), direction);
    result.positionPdf = 1.f / light.area;
    result.directionPdf = directionPdf;

    // 对余弦半球采样，解析约掉 cos(theta)，避免掠射方向上的小量相除。
    result.weight = emitted * (M_PI * light.area);
    return any(emitted > 0.f) && all(isfinite(result.weight)) &&
        isfinite(result.positionPdf) && result.directionPdf > 0.f;
}

// 给定传播方向，在场景包围球的上游圆盘上均匀采点。
bool sampleInfiniteLightOrigin(
    float3 direction, float3 sceneCenter, float sceneRadius, float2 u, inout LightEmissionSample result)
{
    if (!(sceneRadius > 0.f) || !isfinite(sceneRadius))
        return false;

    bool frameValid;
    const ShadingFrame frame = ShadingFrame::createSafe(direction, float4(0.f, 0.f, 0.f, 1.f), frameValid);

    const float2 disk = sceneRadius * sample_disk(u);
    result.posW = sceneCenter + frame.fromLocal(float3(disk, -sceneRadius));
    result.normalW = direction;
    result.positionPdf = 1.f / (M_PI * sceneRadius * sceneRadius);
    result.ray = Ray(result.posW, direction);
    return isfinite(result.positionPdf);
}

// 对应官方 samplePointLight()，发射端在球面或聚光锥内采样方向。
bool samplePointLightEmission(const LightData light, float2 u, out LightEmissionSample result)
{
    result = {};
    const float solidAngle = M_2PI * (1.f - light.cosOpeningAngle);
    if (!(solidAngle > 0.f))
        return false; // 零张角聚光灯的总通量为零。

    bool frameValid;
    const ShadingFrame frame = ShadingFrame::createSafe(light.dirW, float4(0.f, 0.f, 0.f, 1.f), frameValid);
    const float3 localDirection = sample_cone(u, light.cosOpeningAngle);
    const float3 direction = frame.fromLocal(localDirection);
    const float falloff = light.penumbraAngle > 0.f
        ? smoothstep(0.f, light.penumbraAngle, light.openingAngle - acos(clamp(localDirection.z, -1.f, 1.f)))
        : 1.f;

    result.posW = light.posW;
    result.normalW = light.dirW;
    result.ray = Ray(light.posW, direction);
    result.positionPdf = 1.f;
    result.directionPdf = 1.f / solidAngle;
    result.deltaPosition = true;
    result.weight = light.intensity * (falloff * solidAngle);
    return any(result.weight > 0.f) && all(isfinite(result.weight));
}

bool sampleDirectionalLightEmission(
    const LightData light, float3 sceneCenter, float sceneRadius, float2 u, out LightEmissionSample result)
{
    result = {};
    if (!sampleInfiniteLightOrigin(light.dirW, sceneCenter, sceneRadius, u, result))
        return false;

    result.deltaDirection = true;
    result.directionPdf = 1.f;
    result.weight = light.intensity / result.positionPdf;
    return any(result.weight > 0.f) && all(isfinite(result.weight));
}

bool sampleDistantLightEmission(
    const LightData light, float3 sceneCenter, float sceneRadius, float2 uDirection, float2 uPosition,
    out LightEmissionSample result)
{
    result = {};
    const float solidAngle = M_2PI * (1.f - light.cosSubtendedAngle);
    const float3 localDirection = solidAngle > 0.f
        ? sample_cone(uDirection, light.cosSubtendedAngle) : float3(0.f, 0.f, 1.f);
    const float3 direction = -normalize(mul((float3x3)light.transMat, localDirection));
    if (!sampleInfiniteLightOrigin(direction, sceneCenter, sceneRadius, uPosition, result))
        return false;

    result.deltaDirection = !(solidAngle > 0.f);
    result.directionPdf = result.deltaDirection ? 1.f : 1.f / solidAngle;
    // 与官方 sampleDistantLight() 一致，intensity 已包含方向 PDF 的归一化。
    result.weight = light.intensity / result.positionPdf;
    return any(result.weight > 0.f) && all(isfinite(result.weight));
}

// 对应官方 finalizeAreaLightSample()，共用面积变换和发射方向采样。
bool finalizeAreaLightEmission(
    const LightData light, float3 localPosition, float3 localNormal, float localArea, float2 uDirection,
    out LightEmissionSample result)
{
    result = {};
    const float3 worldNormal = mul(light.transMatIT, float4(localNormal, 0.f)).xyz;
    const float normalLength = length(worldNormal);
    const float areaJacobian = abs(determinant((float3x3)light.transMat)) * normalLength;
    if (!(areaJacobian > 0.f) || !isfinite(areaJacobian))
        return false;

    result.posW = mul(light.transMat, float4(localPosition, 1.f)).xyz;
    result.normalW = worldNormal / normalLength;
    result.positionPdf = 1.f / (localArea * areaJacobian);
    bool frameValid;
    const ShadingFrame frame = ShadingFrame::createSafe(result.normalW, float4(0.f, 0.f, 0.f, 1.f), frameValid);
    const float3 localDirection = sample_cosine_hemisphere_polar(uDirection, result.directionPdf);
    result.ray = Ray(computeRayOrigin(result.posW, result.normalW), frame.fromLocal(localDirection));
    result.weight = light.intensity * (M_PI / result.positionPdf);
    return any(result.weight > 0.f) && all(isfinite(result.weight));
}

bool sampleRectAreaLightEmission(const LightData light, float2 uPosition, float2 uDirection, out LightEmissionSample result)
{
    return finalizeAreaLightEmission(light, float3(2.f * uPosition - 1.f, 0.f), float3(0.f, 0.f, 1.f), 4.f, uDirection, result);
}

bool sampleDiscAreaLightEmission(const LightData light, float2 uPosition, float2 uDirection, out LightEmissionSample result)
{
    return finalizeAreaLightEmission(light, float3(sample_disk(uPosition), 0.f), float3(0.f, 0.f, 1.f), M_PI, uDirection, result);
}

bool sampleSphereAreaLightEmission(const LightData light, float2 uPosition, float2 uDirection, out LightEmissionSample result)
{
    const float3 position = sample_sphere(uPosition);
    return finalizeAreaLightEmission(light, position, position, M_4PI, uDirection, result);
}

// 对应官方 sampleLight(shadingPosW, light, sg, ls)，对已经选定的解析光源采样发射。
// sceneCenter/sceneRadius 仅供无限远光源确定发射圆盘，不参与选灯。
bool sampleLightEmission<S : ISampleGenerator>(
    const LightData light, float3 sceneCenter, float sceneRadius, inout S sg, out LightEmissionSample result)
{
    switch (LightType(light.type))
    {
    case LightType::Point:
        return samplePointLightEmission(light, sampleNext2D(sg), result);
    case LightType::Directional:
        return sampleDirectionalLightEmission(light, sceneCenter, sceneRadius, sampleNext2D(sg), result);
    case LightType::Distant:
    {
        const float2 uDirection = light.cosSubtendedAngle < 1.f ? sampleNext2D(sg) : float2(0.f);
        const float2 uPosition = sampleNext2D(sg);
        return sampleDistantLightEmission(light, sceneCenter, sceneRadius, uDirection, uPosition, result);
    }
    case LightType::Rect:
    {
        const float2 uPosition = sampleNext2D(sg);
        return sampleRectAreaLightEmission(light, uPosition, sampleNext2D(sg), result);
    }
    case LightType::Disc:
    {
        const float2 uPosition = sampleNext2D(sg);
        return sampleDiscAreaLightEmission(light, uPosition, sampleNext2D(sg), result);
    }
    case LightType::Sphere:
    {
        const float2 uPosition = sampleNext2D(sg);
        return sampleSphereAreaLightEmission(light, uPosition, sampleNext2D(sg), result);
    }
    default:
        result = {};
        return false;
    }
}
// 环境采样入口保持独立，方向采样复用官方 EnvMapSampler。
bool sampleEnvMapEmission<S : ISampleGenerator>(
    const EnvMapSampler envSampler, float3 sceneCenter, float sceneRadius, inout S sg, out LightEmissionSample result)
{
    result = {};
    // 官方采样器对全黑重要性图没有除零保护，提前将其作为零贡献处理。
    if (!(envSampler.importanceMap.Load(int3(0, 0, envSampler.importanceBaseMip)) > 0.f))
        return false;

    EnvMapSample sample;
    if (!envSampler.sample(sampleNext2D(sg), sample) || !(sample.pdf > 0.f) || !isfinite(sample.pdf))
        return false;
    if (!sampleInfiniteLightOrigin(-sample.dir, sceneCenter, sceneRadius, sampleNext2D(sg), result))
        return false;

    result.directionPdf = sample.pdf;
    result.weight = sample.Le / (sample.pdf * result.positionPdf);
    return any(sample.Le > 0.f) && all(isfinite(result.weight));
}

