import MyRenderPasses.SignificanceCache.CacheTypes;
import MyRenderPasses.SignificanceCache.CosineLobe;
import Utils.Sampling.UniformSampleGenerator;

cbuffer CB
{
    uint gCount;
    uint gExponent;
    float gCosine;
}

RWStructuredBuffer<float4> gResults;

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gCount)
        return;
    UniformSampleGenerator sg = UniformSampleGenerator(uint2(tid.x, 0u), 512u);
    const float3 axis = float3(sqrt(saturate(1.f - gCosine * gCosine)), 0.f, gCosine);
    const float3 w = sampleClippedCosineLobe(axis, float3(0.f, 0.f, 1.f), gExponent, sg);
    const bool invalid = !all(isfinite(w)) || w.z <= 0.f || abs(dot(w, w) - 1.f) > 1e-5f || dot(w, axis) < 0.f;
    gResults[tid.x] = float4(w.z, dot(w, axis), clippedLobeMass(gExponent, gCosine), invalid ? 1.f : 0.f);
}
