#include "PathReuseParams.slangh"

import Scene.RaytracingInline;
import Utils.Math.BitTricks;
import Utils.Math.HashUtils;
import Utils.Math.Ray;
import Utils.Sampling.SampleGenerator;
import PathReuseSample;

static const uint kGroupThreadCount = kPathReuseThreadsPerAxis * kPathReuseThreadsPerAxis;
static const bool kUseImportanceSampling = USE_IMPORTANCE_SAMPLING;

[shader("compute")]
[numthreads(kPathReuseThreadsPerAxis, kPathReuseThreadsPerAxis, 1)]
void main(uint threadIndex : SV_GroupIndex, uint3 groupID : SV_GroupID)
{
    const ReuseGroup group = ReuseGroup(groupID);

    // 对每个 a1 → b2，记录可见性、贡献和联合密度。
    for (uint pair = threadIndex; pair < kPathReuseGroupSize * kPathReuseGroupSize; pair += kGroupThreadCount)
        group.evaluatePair(pair / kPathReuseGroupSize, pair % kPathReuseGroupSize);
    GroupMemoryBarrierWithGroupSync();

    // 按 B 的路径分别归约 BSDF 和 NEE 的联合密度。
    if (threadIndex < kPathReuseGroupSize)
        group.sumProposalPdfs(threadIndex);
    GroupMemoryBarrierWithGroupSync();

    // 给 A 像素累加本地光、原路径和可见的邻居路径。
    if (threadIndex < kPathReuseGroupSize)
        group.writePixel(threadIndex);
}

struct ProposalPdfSums
{
    float bsdf;
    float nee;
};

struct ReconnectionTerm
{
    float3 integrand;
    float proposalPdf; // 以面积乘立体角为度量的联合密度。
};

struct ReconnectionEvaluation
{
    ReconnectionTerm bsdf;
    ReconnectionTerm nee;

    static ReconnectionEvaluation original(const SecondaryPathSample sample)
    {
        const float pdfA = sample.primaryToSecondaryPdfA;
        const float bsdfPdf = pdfA * sample.bsdf.pdfW;
        const float neePdf = pdfA * sample.nee.pdfW;

        // 原始估计是 F / q，乘回 q 后和重连项一样存 F。
        ReconnectionEvaluation evaluation;
        evaluation.bsdf = {sample.bsdf.originalEstimate() * bsdfPdf, bsdfPdf};
        evaluation.nee = {sample.nee.originalEstimate() * neePdf, neePdf};
        return evaluation;
    }
};

struct SurfaceConnection
{
    ShadingData a1Shading;
    ShadingData b2Shading;
    float3 connectionDirW; // a1 → b2。
    float distanceSquared;
    bool reuseBsdf;
    bool reuseNee;

    [mutating]
    bool connect(const PrimaryVertexSample a1, const SecondaryPathSample b2)
    {
        a1Shading = loadTriangleShadingData(a1.hit.getTriangleHit(), a1.viewW);
        const TriangleHit b2Hit = b2.hit.getTriangleHit();
        const VertexData b2Vertex = gScene.getVertexData(b2Hit);
        const float3 segment = b2Vertex.posW - a1Shading.posW;
        distanceSquared = dot(segment, segment);
        if (distanceSquared <= 0.f)
            return false;

        connectionDirW = segment * rsqrt(distanceSquared);
        b2Shading = gScene.materials.prepareShadingData(b2Vertex, gScene.getMaterialID(b2Hit.instanceID), -connectionDirW);
        if (!isReflectionDirection(a1Shading, connectionDirW))
            return false;

        reuseBsdf = b2.bsdf.pdfW > 0.f && isReflectionDirection(b2Shading, b2.bsdf.directionW);
        reuseNee = b2.nee.pdfW > 0.f && isReflectionDirection(b2Shading, b2.nee.directionW);
        if (!reuseBsdf && !reuseNee)
            return false;
        return isVisible();
    }

    bool isVisible()
    {
        const float3 origin = a1Shading.computeRayOrigin();
        const float3 segment = b2Shading.computeRayOrigin() - origin;
        if (dot(segment, connectionDirW) <= 0.f)
            return false;

        const float distance = length(segment);
        SceneRayQuery<1> rayQuery;
        return rayQuery.traceVisibilityRay(Ray(origin, segment / distance, 0.f, distance));
    }

    ReconnectionEvaluation evaluate(const SecondaryPathSample b2, inout SampleGenerator sg)
    {
        ReconnectionEvaluation result = {};
        let lod = ExplicitLodTextureSampler(0.f);
        let a1Material = gScene.materials.getMaterialInstance(a1Shading, lod);
        let b2Material = gScene.materials.getMaterialInstance(b2Shading, lod);
        const float jacobian = solidAngleToAreaJacobian(b2Shading.faceN, connectionDirW, distanceSquared);
        const float pdfA = evalBsdfPdfW(a1Shading, a1Material, connectionDirW, kUseImportanceSampling) * jacobian;
        const bool canSampleNee = hasContinuousLobes(b2Shading, b2Material);

        // Falcor 的 eval 已包含各自出射方向的余弦项。
        const float3 a1Factor = a1Material.eval(a1Shading, connectionDirW, sg) * jacobian;
        if (reuseBsdf)
        {
            const SuffixEvaluation suffix = b2.bsdf.evaluate(b2Shading, b2Material, sg, kUseImportanceSampling, canSampleNee);
            result.bsdf = {a1Factor * suffix.integrand, pdfA * suffix.pdfW};
        }
        if (reuseNee && canSampleNee)
        {
            const SuffixEvaluation suffix = b2.nee.evaluate(b2Shading, b2Material, sg, kUseImportanceSampling);
            result.nee = {a1Factor * suffix.integrand, pdfA * suffix.pdfW};
        }
        return result;
    }
};

StructuredBuffer<PathReuseSample> gPathSamples;
RWTexture2D<float4> gOutputColor;
ConstantBuffer<ReusePathsParams> gReuseParams;

// A 提供 a1，B 提供 b2 和后缀。矩阵按 [aIndex][bIndex] 索引，每格都保存 F 和联合 PDF。
groupshared uint gVisibility[kPathReuseGroupSize][kPathReuseGroupSize];
groupshared ReconnectionEvaluation gReconnectionEvaluations[kPathReuseGroupSize][kPathReuseGroupSize];
groupshared ProposalPdfSums gProposalPdfSums[kPathReuseGroupSize];

struct ReuseGroup
{
    uint2 frameDim;
    int2 tileOrigin;
    uint rookGroup;
    uint2 permutationSeeds;
    bool useNRooks;

    __init(uint3 groupID)
    {
        gOutputColor.GetDimensions(frameDim.x, frameDim.y);
        useNRooks = gReuseParams.useNRooks != 0u;
        const uint tileSize = useNRooks ? kPathReuseGroupSize : kPathReuseTileSide;
        tileOrigin = int2(groupID.xy * tileSize) - int2(gReuseParams.tileOffset);
        rookGroup = groupID.z;
        permutationSeeds = uint2(0u);
        if (useNRooks)
        {
            permutationSeeds.x = jenkinsHash(interleave_32bit(groupID.xy) ^ jenkinsHash(gReuseParams.frameIndex));
            permutationSeeds.y = jenkinsHash(permutationSeeds.x ^ 0x9e3779b9u);
        }
    }

    static uint permuteRookIndex(uint index, uint seed)
    {
        // 每步用一个 2 位半部对另一半做异或，保持 0 到 15 之间的一一映射。
        uint low = index & 3u;
        uint high = index >> 2u;
        low ^= (seed >> (2u * high)) & 3u;
        high ^= (seed >> (8u + 2u * low)) & 3u;
        low ^= (seed >> (16u + 2u * high)) & 3u;
        return low | (high << 2u);
    }

    uint2 pixel(uint member)
    {
        if (!useNRooks)
            return uint2(tileOrigin + int2(member % kPathReuseTileSide, member / kPathReuseTileSide));

        // 同一块内共用排列，循环平移列序号得到互不重叠的 N-rooks。
        const uint shuffledRow = permuteRookIndex(member, permutationSeeds.x);
        const uint column = permuteRookIndex((shuffledRow + rookGroup) % kPathReuseGroupSize, permutationSeeds.y);
        return uint2(tileOrigin + int2(column, member));
    }

    uint pathIndex(uint2 pixelPosition)
    {
        return pixelPosition.y * frameDim.x + pixelPosition.x;
    }

    void evaluatePair(uint aIndex, uint bIndex)
    {
        gVisibility[aIndex][bIndex] = 0u;

        const uint2 aPixel = pixel(aIndex);
        const uint2 bPixel = pixel(bIndex);
        // 移出画面的负坐标转为 uint 后也会被排除。
        if (any(aPixel >= frameDim) || any(bPixel >= frameDim))
            return;

        const SecondaryPathSample b2 = gPathSamples[pathIndex(bPixel)].secondary;

        // 路径需要至少一条可复用的 BSDF 或 NEE 后缀。
        if (!b2.hasReusableSuffix())
            return;

        if (aIndex == bIndex)
        {
            gReconnectionEvaluations[aIndex][bIndex] = ReconnectionEvaluation.original(b2);
            gVisibility[aIndex][bIndex] = 1u;
            return;
        }

        const PrimaryVertexSample a1 = gPathSamples[pathIndex(aPixel)].primary;
        if (!a1.hit.isValid())
            return;

        SurfaceConnection connection = {};
        if (!connection.connect(a1, b2))
            return;

        // 与第一阶段的采样使用不同的随机序列。
        const uint sampleNumber = (gReuseParams.frameIndex * kPathReuseGroupSize + bIndex) | 0x80000000u;
        SampleGenerator sg = SampleGenerator(aPixel, sampleNumber);
        gVisibility[aIndex][bIndex] = 1u;
        gReconnectionEvaluations[aIndex][bIndex] = connection.evaluate(b2, sg);
    }

    void sumProposalPdfs(uint a2)
    {
        ProposalPdfSums sums = {};
        for (uint i = 0; i < kPathReuseGroupSize; ++i)
        {
            if (gVisibility[i][a2] == 0u)
                continue;

            const ReconnectionEvaluation evaluation = gReconnectionEvaluations[i][a2];
            sums.bsdf += evaluation.bsdf.proposalPdf;
            sums.nee += evaluation.nee.proposalPdf;
        }
        gProposalPdfSums[a2] = sums;
    }

    void writePixel(uint aIndex)
    {
        const uint2 aPixel = pixel(aIndex);
        if (any(aPixel >= frameDim))
            return;

        float3 color = gPathSamples[pathIndex(aPixel)].localRadiance;
        for (uint bIndex = 0; bIndex < kPathReuseGroupSize; ++bIndex)
        {
            if (gVisibility[aIndex][bIndex] == 0u)
                continue;

            const ReconnectionEvaluation evaluation = gReconnectionEvaluations[aIndex][bIndex];
            const ProposalPdfSums sums = gProposalPdfSums[bIndex];
            color += sums.bsdf > 0.f ? evaluation.bsdf.integrand / sums.bsdf : float3(0.f);
            color += sums.nee > 0.f ? evaluation.nee.integrand / sums.nee : float3(0.f);
        }
        gOutputColor[aPixel] = float4(color, 1.f);
    }
};
