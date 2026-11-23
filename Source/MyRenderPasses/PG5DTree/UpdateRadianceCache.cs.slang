import PG5DTreeCache;
import Utils.Color.ColorHelpers;

cbuffer UpdateCB
{
    uint gCellCount;
    uint gBinCount;
    uint gMinCellSamples;
}

ByteAddressBuffer gTrainingBins;
Buffer<uint> gTrainingCounts;
RWStructuredBuffer<RadianceCacheBin> gCacheBins;
RWStructuredBuffer<RadianceCacheCell> gCacheCells;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 thread : SV_DispatchThreadID)
{
    const uint cellIndex = thread.x;
    if (cellIndex >= gCellCount)
        return;

    RadianceCacheCell cell = {};
    if (gTrainingCounts[cellIndex] >= gMinCellSamples)
    {
        cell.hasEnoughSamples = 1u;
        const uint firstBin = cellIndex * gBinCount;
        float3 sumRadiance = float3(0.f);
        float total = 0.f;
        for (uint i = 0; i < gBinCount; ++i)
        {
            const uint index = firstBin + i;
            const float4 sums = asfloat(gTrainingBins.Load4(index * kTrainingBinStride));
            RadianceCacheBin bin = {};
            if (sums.w > 0.f && all(isfinite(sums)))
                bin.radiance = max(sums.xyz / sums.w, 0.f);
            sumRadiance += bin.radiance;
            total += luminance(bin.radiance);
            bin.cdf = total;
            gCacheBins[index] = bin;
        }

        if (total > 0.f && isfinite(total) && all(isfinite(sumRadiance)))
        {
            cell.meanRadiance = sumRadiance / gBinCount;
            for (uint i = 0; i < gBinCount; ++i)
            {
                const uint index = firstBin + i;
                RadianceCacheBin bin = gCacheBins[index];
                bin.cdf = i + 1u == gBinCount ? 1.f : bin.cdf / total;
                gCacheBins[index] = bin;
            }
        }
    }
    gCacheCells[cellIndex] = cell;
}
