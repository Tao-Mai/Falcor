import Utils.Geometry.GeometryHelpers;
import Utils.Math.MathHelpers;
import Utils.Sampling.SampleGeneratorInterface;

#include "Utils/Math/MathConstants.slangh"

// 对给定轴的幂余弦波瓣进行拒绝采样，返回表面法线上半球中的方向。
// 调用者保证 dot(axis, normal) >= 0，因此接受率至少为 1/2。
float3 sampleClippedCosineLobe<S : ISampleGenerator>(float3 axis, float3 normal, uint exponent, inout S sg)
{
    float3 tangent, bitangent;
    branchlessONB(axis, tangent, bitangent);
    for (;;)
    {
        const float2 u = sampleNext2D(sg);
        const float z = pow(u.x, 1.f / float(exponent + 1u));
        const float r = sqrt(saturate(1.f - z * z));
        const float phi = M_2PI * u.y;
        const float3 direction = tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + axis * z;
        if (dot(normal, direction) > 0.f)
            return direction;
    }
}
