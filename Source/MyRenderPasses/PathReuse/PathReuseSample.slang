#include "Utils/Math/MathConstants.slangh"

import Scene.Shading;
import Utils.Sampling.SampleGenerator;

float powerHeuristic(float pdf, float otherPdf)
{
    if (!(otherPdf > 0.f))
        return 1.f;
    const float ratio = otherPdf / pdf;
    return 1.f / (1.f + ratio * ratio);
}

// 论文中的 x_i 和 y_j 分别对应主命中和第二个顶点。
// Bekaert 等，Accelerating Path Tracing by Re-Using Paths，式 (12) 和第 3.4 节。
struct PrimaryVertexSample
{
    HitInfo hit;
    float3 viewW; // 从第一个顶点指向相机。
};

struct ContinuationRadiance
{
    float3 continuation; // 环境光以及从第三个顶点继续散射得到的辐亮度。
    float3 emission; // 第三个顶点自身发出的辐亮度。
    float lightPdfW; // 通过 NEE 采到这个发光命中的立体角 PDF。

    float3 withLocalMis(float bsdfPdfW, bool canSampleNee = true)
    {
        const float competingPdfW = canSampleNee ? lightPdfW : 0.f;
        return continuation + emission * powerHeuristic(bsdfPdfW, competingPdfW);
    }
};

struct SuffixEvaluation
{
    float3 integrand; // 后缀的被积函数，尚未除方向 PDF。
    float pdfW; // 生成这个方向的实际采样密度。
};

struct BsdfContinuation
{
    float3 directionW;
    float pdfW; // 第二次散射的立体角 PDF，零表示没有可复用样本。
    ContinuationRadiance radiance;
    float3 throughput; // 原路径前两次散射的采样权重。

    float3 originalEstimate()
    {
        return throughput * radiance.withLocalMis(pdfW);
    }

    SuffixEvaluation evaluate(
        const ShadingData b2, const IMaterialInstance material, inout SampleGenerator sg,
        bool useImportanceSampling, bool canSampleNee
    )
    {
        const float directionPdfW = evalBsdfPdfW(b2, material, directionW, useImportanceSampling);
        const float3 scatter = material.eval(b2, directionW, sg);
        return {scatter * radiance.withLocalMis(directionPdfW, canSampleNee), directionPdfW};
    }
};

struct NeeSample
{
    float3 directionW;
    float pdfW; // 从第二个顶点采到灯点的立体角 PDF，零表示没有可复用样本。
    float3 emission; // 通过 alpha test 和可见性查询的灯点辐亮度。
    float bsdfPdfW; // 原路径在第二个顶点通过 BSDF 生成同方向的立体角 PDF。
    float3 sampledContribution; // 原像素的直接采光估计，含原始采样权重。

    float3 originalEstimate()
    {
        return sampledContribution * powerHeuristic(pdfW, bsdfPdfW);
    }

    SuffixEvaluation evaluate(
        const ShadingData b2, const IMaterialInstance material, inout SampleGenerator sg, bool useImportanceSampling
    )
    {
        const float competingPdfW = evalBsdfPdfW(b2, material, directionW, useImportanceSampling);
        const float3 scatter = material.eval(b2, directionW, sg);
        const float weight = powerHeuristic(pdfW, competingPdfW);
        return {scatter * emission * weight, pdfW};
    }
};

struct SecondaryPathSample
{
    HitInfo hit;
    float primaryToSecondaryPdfA; // 原始主顶点采到第二个顶点的面积 PDF。
    BsdfContinuation bsdf;
    NeeSample nee;

    bool hasReusableSuffix()
    {
        return hit.isValid() && (bsdf.pdfW != 0.f || nee.pdfW != 0.f);
    }

    bool supportsSuffixPdf(float pdfW)
    {
        const float pathPdf = primaryToSecondaryPdfA * pdfW;
        return pathPdf > 0.f && pathPdf < FLT_MAX;
    }
};

struct PathReuseSample
{
    PrimaryVertexSample primary;
    SecondaryPathSample secondary;
    float3 localRadiance; // 相机背景、局部采光以及逐像素累积的路径贡献。
};

ShadingData loadTriangleShadingData(const TriangleHit hit, float3 viewW)
{
    return gScene.materials.prepareShadingData(gScene.getVertexData(hit), gScene.getMaterialID(hit.instanceID), viewW);
}

float solidAngleToAreaJacobian(float3 faceNormalW, float3 directionW, float distanceSquared)
{
    return abs(dot(faceNormalW, directionW)) / distanceSquared;
}

bool isReflectionDirection(const ShadingData sd, float3 direction)
{
    return dot(sd.getOrientedFaceNormal(), direction) > 0.f;
}

bool hasContinuousLobes(const ShadingData sd, const IMaterialInstance mi)
{
    return (mi.getLobeTypes(sd) & uint(LobeType::NonDelta)) != 0;
}

float evalBsdfPdfW(const ShadingData sd, const IMaterialInstance mi, float3 direction, bool useImportanceSampling)
{
    // 参考余弦采样对透射方向可能返回带符号的值。
    return abs(mi.evalPdf(sd, direction, useImportanceSampling));
}
