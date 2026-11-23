// 训练缓冲每个方向箱存 RGB 加权和与采样权重和，共四个 32 位值。
static const uint kTrainingBinStride = 16u;
static const uint kInvalidCacheIndex = 0xffffffffu;

struct RadianceCacheBin
{
    float3 radiance;
    float cdf;
};

struct RadianceCacheCell
{
    float3 meanRadiance;
    uint hasEnoughSamples;
};
