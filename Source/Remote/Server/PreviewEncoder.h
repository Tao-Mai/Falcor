#pragma once

#include "Falcor.h"
#include <memory>
#include <vector>

namespace Falcor::Remote
{
// Wire IDs shared with the Python preview decoder.
enum class PreviewCodec : uint32_t
{
    H264 = 1,
    Hevc,
    HevcRgb,
    Jpeg,
};

struct PreviewEncoding
{
    PreviewCodec codec = PreviewCodec::H264;
    uint32_t fps = 60;
    uint32_t bitrateMbps = 15;
    uint32_t jpegQuality = 90;
    bool jpeg444 = true;
};

struct EncodeTimings
{
    double inputMs;
    double encodeMs;
};

class PreviewEncoder
{
public:
    PreviewEncoder(ref<Device> device, uint2 size, const PreviewEncoding& settings);
    ~PreviewEncoder();

    // Append one frame from an 8-bit BGRA render target to the caller's buffer.
    EncodeTimings encode(RenderContext& context, const Texture& texture, bool keyFrame, std::vector<uint8_t>& output);

private:
    struct Impl;
    std::unique_ptr<Impl> mpImpl;
};
} // namespace Falcor::Remote
