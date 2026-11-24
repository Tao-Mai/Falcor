import CacheTypes;
#include "Utils/Math/MathConstants.slangh"

cbuffer UpdateCB
{
    uint gCellCount;
    float gMaxExponent;
    float gRadiusSquared;
}

RWStructuredBuffer<CacheRecord> gRecords;
StructuredBuffer<CacheSample> gSamples;
RWStructuredBuffer<uint> gSampleCounts;

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint cell = tid.x;
    if (cell >= gCellCount)
        return;

    const uint sampleCount = min(gSampleCounts[cell], kSamplesPerCell);
    // 每格单线程串行执行论文 Algorithm 1。其它格不会写入这些波瓣。
    for (uint s = 0u; s < sampleCount; ++s)
    {
        const CacheSample sample = gSamples[cell * kSamplesPerCell + s];
        uint useful = 0u;
        uint freeSlot = kLobesPerCell;
        for (uint i = 0u; i < kLobesPerCell; ++i)
        {
            const uint index = cell * kLobesPerCell + i;
            CacheRecord record = gRecords[index];
            if (record.count == 0u)
            {
                freeSlot = min(freeSlot, i);
                continue;
            }
            const float3 delta = record.position - sample.position;
            if (dot(delta, delta) > gRadiusSquared || dot(record.direction, sample.normal) <= 0.f)
                continue;

            const float relevance = pow(saturate(dot(record.direction, sample.direction)), record.exponent);
            const float increment = 1.f - relevance;
            record.totalSamples += increment;
            if (abs(record.radiance - sample.radiance) < 0.5f * record.radiance)
                record.usefulSamples += increment;
            record.exponent = clamp(record.totalSamples / max(record.usefulSamples, 1e-6f) - 1.f, 1.f, gMaxExponent);
            record.count = min(record.count + 1u, 1u << 20);
            const float rate = 1.f / float(record.count);
            record.meanRadiance = lerp(record.meanRadiance, sample.radiance, rate);
            if (sample.radiance < record.meanRadiance)
                ++useful;
            else if (relevance >= 0.5f)
            {
                record.radiance = lerp(record.radiance, sample.radiance, rate);
                record.direction = normalize(lerp(record.direction, sample.direction, rate));
                ++useful;
            }
            gRecords[index] = record;
        }

        if (useful == 0u && sample.radiance > 0.f && freeSlot < kLobesPerCell)
        {
            CacheRecord record = {};
            record.position = sample.position;
            record.direction = sample.direction;
            record.normal = sample.normal;
            record.radiance = sample.radiance;
            record.meanRadiance = sample.radiance;
            record.exponent = 1.f;
            record.count = 1u;
            // 两个小先验使新波瓣第一次更新时具有有限指数。
            record.usefulSamples = 1.f;
            record.totalSamples = 2.f;
            gRecords[cell * kLobesPerCell + freeSlot] = record;
        }
    }
    for (uint i = 0u; i < kLobesPerCell; ++i)
    {
        const uint index = cell * kLobesPerCell + i;
        CacheRecord record = gRecords[index];
        if (record.count != 0u)
        {
            const uint exponent = uint(ceil(record.exponent));
            record.normalization = float(exponent + 1u) /
                (M_2PI * clippedLobeMass(exponent, dot(record.normal, record.direction)));
            gRecords[index] = record;
        }
    }
    gSampleCounts[cell] = 0u;
}
