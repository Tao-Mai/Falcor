#include "Utils/Math/MathConstants.slangh"

// 每格最多 8 个连续波瓣，默认查询最近 4 个。训练样本按批次有界存储。
static const uint kLobesPerCell = 8;
#ifndef SC_QUERY_LOBES
#define SC_QUERY_LOBES 4
#endif
static const uint kQueryLobes = SC_QUERY_LOBES;
static const uint kSamplesPerCell = 32;
#ifndef SC_PATH_CAPACITY
#define SC_PATH_CAPACITY 18
#endif
static const uint kPathCapacity = SC_PATH_CAPACITY;

struct CacheRecord
{
    float3 position;
    float radiance;
    float3 direction;
    float exponent;
    float meanRadiance;
    float usefulSamples;
    float totalSamples;
    uint count;
    float3 normal;
    float normalization;
};

struct CacheSample
{
    float3 position;
    float radiance;
    float3 direction;
    uint valid;
    float3 normal;
    uint hasEmissiveNEE;
};

struct CacheGrid
{
    float3 origin;
    uint resolution;
    float3 invExtent;
    float radiusSquared;
};

uint3 cellCoordinate(CacheGrid grid, float3 position)
{
    return min(uint3(saturate((position - grid.origin) * grid.invExtent) * grid.resolution), grid.resolution - 1u);
}

uint cellIndex(CacheGrid grid, uint3 coordinate)
{
    return (coordinate.z * grid.resolution + coordinate.y) * grid.resolution + coordinate.x;
}

// 对整数幂 n，精确积分倾斜余弦波瓣在表面半球内的质量。
// A_n(c) = 1/2 + 1/2 I_(c*c)(1/2, (n+1)/2)，c >= 0。
// 使用积分递推，避免数值求积或把截断波瓣误当作完整半球。
float clippedLobeMass(uint exponent, float cosine)
{
    const float c = saturate(cosine);
    const float s2 = saturate(1.f - c * c);
    const bool odd = (exponent & 1u) != 0u;
    float mass = odd ? 0.5f + 0.5f * c : 0.5f + asin(c) / M_PI;
    float coefficient = odd ? 0.5f : 1.f / M_PI;
    float power = odd ? s2 : sqrt(s2);
    for (uint n = odd ? 3u : 2u; n <= exponent; n += 2u)
    {
        mass += coefficient * c * power / float(n - 1u);
        coefficient *= float(n) / float(n - 1u);
        power *= s2;
    }
    return min(mass, 1.f);
}
