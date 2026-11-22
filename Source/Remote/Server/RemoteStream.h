#pragma once

#include "Falcor.h"
#include "RemoteTransport.h"
#include <nlohmann/json_fwd.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace Falcor::Remote
{
class RemoteStream
{
public:
    using InputHandler = std::function<void(const InputEvent&)>;
    using CommandHandler = std::function<nlohmann::json(const nlohmann::json&)>;

    RemoteStream(ref<Device> device, uint16_t port, uint32_t fps);
    ~RemoteStream();

    void poll(const InputHandler& onInput, const CommandHandler& onCommand);
    bool connected() const { return bool(mVideo); }
    bool ready() const;
    void present(RenderContext& context, const Texture& texture);

    uint32_t frameLimit() const { return mFps; }
    double framesPerSecond() const { return mStreamFps; }
    double gpuWaitMs() const { return mGpuWaitMs; }
    double encodeMs() const { return mEncodeMs; }

private:
    using Clock = std::chrono::steady_clock;
    struct Encoder;
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

    ref<Device> mpDevice;
    Socket mListener;
    Socket mVideo;
    Socket mControl;
    std::vector<Peer> mPeers;
    uint64_t mSession = 0;
    uint64_t mControlSession = 0;
    bool mRequestIdr = true;
    InputPacket mInput{};
    size_t mInputOffset = 0;

    uint32_t mCommandSize = 0;
    size_t mCommandSizeOffset = 0;
    std::vector<uint8_t> mCommand;
    size_t mCommandOffset = 0;
    std::vector<uint8_t> mReply;
    size_t mReplyOffset = 0;

    const uint32_t mFps;
    std::unique_ptr<Encoder> mpEncoder;
    std::vector<uint8_t> mFrame;
    size_t mFrameOffset = 0;
    Clock::time_point mNextFrame{};
    Clock::time_point mFpsStart{};
    uint32_t mEncodedFrames = 0;
    double mStreamFps = 0;
    double mGpuWaitMs = 0;
    double mEncodeMs = 0;
};
} // namespace Falcor::Remote
