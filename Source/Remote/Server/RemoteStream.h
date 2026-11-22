#pragma once

#include "PreviewEncoder.h"
#include "RemoteTransport.h"
#include <nlohmann/json_fwd.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Falcor::Remote
{
class RemoteStream
{
public:
    using InputHandler = std::function<void(const InputEvent&)>;
    using CommandHandler = std::function<nlohmann::json(const nlohmann::json&)>;

    RemoteStream(ref<Device> device, uint16_t port, uint32_t fps, uint2 previewSize);
    ~RemoteStream();

    void poll(const InputHandler& onInput, const CommandHandler& onCommand);
    bool connected() const { return bool(mVideo); }
    bool ready() const;
    void present(RenderContext& context, const Texture& texture);

    uint32_t frameLimit() const { return mFramedVideo ? mEncoding.fps : mMaxFps; }
    double framesPerSecond() const { return mStreamFps; }
    double gpuWaitMs() const { return mGpuWaitMs; }
    double encodeMs() const { return mEncodeMs; }
    nlohmann::json encoding() const;
    const std::string& encodingError() const { return mEncodingError; }

private:
    using Clock = std::chrono::steady_clock;
    struct Peer
    {
        Socket socket;
        InputPacket hello{};
        size_t offset = 0;
        Clock::time_point deadline;
    };

    void acceptPeers(const InputHandler& onInput);
    void pollCommands(const CommandHandler& onCommand);
    void disconnect(const InputHandler& onInput);
    void flushVideo();
    void closeControl();
    void configureEncoding(const nlohmann::json& changes);
    void resetStats();

    ref<Device> mpDevice;
    Socket mListener;
    Socket mVideo;
    Socket mControl;
    std::vector<Peer> mPeers;
    uint64_t mSession = 0;
    uint64_t mControlSession = 0;
    bool mRequestIdr = true;
    bool mFramedVideo = false;
    bool mEncodingFailed = false;
    uint32_t mGeneration = 0;
    InputPacket mInput{};
    size_t mInputOffset = 0;

    uint32_t mCommandSize = 0;
    size_t mCommandSizeOffset = 0;
    std::vector<uint8_t> mCommand;
    size_t mCommandOffset = 0;
    std::vector<uint8_t> mReply;
    size_t mReplyOffset = 0;

    const uint32_t mMaxFps;
    const uint2 mPreviewSize;
    PreviewEncoding mEncoding;
    std::string mEncodingError;
    std::unique_ptr<PreviewEncoder> mpEncoder;
    std::vector<uint8_t> mFrame;
    size_t mFrameOffset = 0;
    Clock::time_point mNextFrame{};
    Clock::time_point mFpsStart{};
    uint32_t mEncodedFrames = 0;
    uint64_t mEncodedBytes = 0;
    double mStreamFps = 0;
    double mStreamMbps = 0;
    double mGpuWaitMs = 0;
    double mEncodeMs = 0;
};
} // namespace Falcor::Remote
