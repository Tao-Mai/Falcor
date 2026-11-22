#include "RemoteStream.h"
#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <algorithm>
#include <cstring>
#include <string_view>

namespace Falcor::Remote
{
using nlohmann::json;

namespace
{
constexpr std::pair<std::string_view, PreviewCodec> kCodecs[] = {
    {"h264", PreviewCodec::H264},
    {"hevc", PreviewCodec::Hevc},
    {"hevc_rgb", PreviewCodec::HevcRgb},
    {"jpeg", PreviewCodec::Jpeg},
};

std::string_view codecName(PreviewCodec codec)
{
    for (const auto& entry : kCodecs)
        if (entry.second == codec)
            return entry.first;
    FALCOR_THROW("Unknown preview codec.");
}

PreviewCodec readCodec(const json& value)
{
    const auto& name = value.get_ref<const std::string&>();
    for (const auto& entry : kCodecs)
        if (entry.first == name)
            return entry.second;
    FALCOR_THROW("Unknown preview codec '{}'.", name);
}

uint32_t readSetting(const json& changes, const char* key, uint32_t previous, uint32_t maximum)
{
    const auto value = changes.find(key);
    if (value == changes.end())
        return previous;
    FALCOR_CHECK(value->is_number_integer(), "'{}' must be an integer.", key);
    const double number = value->get<double>();
    FALCOR_CHECK(number >= 1 && number <= maximum, "'{}' must be between 1 and {}.", key, maximum);
    return uint32_t(number);
}
} // namespace

RemoteStream::RemoteStream(ref<Device> device, uint16_t port, uint32_t fps, uint2 previewSize)
    : mpDevice(std::move(device)), mListener(Socket::listen("127.0.0.1", port)), mMaxFps(fps),
      mPreviewSize(previewSize), mEncoding{PreviewCodec::H264, fps}
{
    logInfo("Falcor Remote listening on 127.0.0.1:{} (video limit {} FPS).", port, fps);
}

RemoteStream::~RemoteStream() = default;

json RemoteStream::encoding() const
{
    if (!mFramedVideo)
        return nullptr;
    return {
        {"codec", codecName(mEncoding.codec)}, {"fps", mEncoding.fps}, {"max_fps", mMaxFps},
        {"bitrate_mbps", mEncoding.bitrateMbps}, {"jpeg_quality", mEncoding.jpegQuality},
        {"jpeg_chroma", mEncoding.jpeg444 ? "444" : "420"}, {"measured_mbps", mStreamMbps}
    };
}

void RemoteStream::configureEncoding(const json& changes)
{
    FALCOR_CHECK(mFramedVideo, "Update the viewer to change preview encoding.");
    FALCOR_CHECK(changes.is_object(), "Provide preview encoding settings.");
    auto next = mEncoding;
    if (changes.contains("codec"))
        next.codec = readCodec(changes.at("codec"));
    next.fps = readSetting(changes, "fps", next.fps, mMaxFps);
    next.bitrateMbps = readSetting(changes, "bitrate_mbps", next.bitrateMbps, 100);
    next.jpegQuality = readSetting(changes, "jpeg_quality", next.jpegQuality, 100);
    if (changes.contains("jpeg_chroma"))
    {
        const auto& sampling = changes.at("jpeg_chroma").get_ref<const std::string&>();
        FALCOR_CHECK(sampling == "444" || sampling == "420", "JPEG chroma sampling must be 4:4:4 or 4:2:0.");
        next.jpeg444 = sampling == "444";
    }
    if (next.codec == mEncoding.codec && next.fps == mEncoding.fps && next.bitrateMbps == mEncoding.bitrateMbps &&
        next.jpegQuality == mEncoding.jpegQuality && next.jpeg444 == mEncoding.jpeg444)
        return;

    // Construct the replacement before committing settings. A queued packet still carries its original format.
    auto encoder = std::make_unique<PreviewEncoder>(mpDevice, mPreviewSize, next);
    mpEncoder = std::move(encoder);
    mEncoding = next;
    mEncodingError.clear();
    mRequestIdr = true;
    ++mGeneration;
    mNextFrame = Clock::now();
    resetStats();
}

void RemoteStream::resetStats()
{
    mFpsStart = Clock::now();
    mEncodedFrames = 0;
    mEncodedBytes = 0;
    mStreamFps = 0;
    mStreamMbps = 0;
}

void RemoteStream::acceptPeers(const InputHandler& onInput)
{
    auto socket = mListener.accept(!mVideo && mPeers.empty() ? 100 : 0);
    if (socket && mPeers.size() < 4)
        mPeers.push_back({std::move(socket), {}, 0, Clock::now() + std::chrono::milliseconds(100)});

    for (auto peer = mPeers.begin(); peer != mPeers.end();)
    {
        const bool hasHello = peer->socket.read(peer->hello.data(), sizeof(peer->hello), peer->offset);
        const bool expired = Clock::now() >= peer->deadline;
        if (peer->socket && !hasHello && !expired)
        {
            ++peer;
            continue;
        }

        const InputEvent hello = unpackInput(peer->hello);
        const uint64_t session = uint64_t(hello.code) << 32 | hello.action;
        if (peer->socket && hasHello && hello.type == InputType::OpenControls)
        {
            if (session && (!mVideo || mSession == session))
            {
                closeControl();
                mControl = std::move(peer->socket);
                mControlSession = session;
            }
        }
        else if (peer->socket && !mVideo && (hasHello || !peer->offset))
        {
            mSession = hasHello && hello.type == InputType::OpenPreview ? session : 0;
            if (mControlSession != mSession)
                closeControl();
            mVideo = std::move(peer->socket);
            mFramedVideo = hasHello && hello.type == InputType::OpenPreview && hello.mods == kPreviewProtocol;
            mRequestIdr = true;
            ++mGeneration;
            mNextFrame = Clock::now();
            resetStats();
            if (mSession)
                onInput({InputType::EnableLookNavigation});
            else if (hasHello)
                onInput(hello);
            logInfo("Remote viewer connected.");
        }
        peer = mPeers.erase(peer);
    }
}

void RemoteStream::closeControl()
{
    mControl.close();
    mControlSession = 0;
    mCommandSize = 0;
    mCommandSizeOffset = mCommandOffset = mReplyOffset = 0;
    mCommand.clear();
    mReply.clear();
}

void RemoteStream::disconnect(const InputHandler& onInput)
{
    onInput({InputType::ReleaseAll, 1});
    closeControl();
    mSession = 0;
    mFrame.clear();
    mInputOffset = mFrameOffset = 0;
    mpEncoder.reset();
    mFramedVideo = false;
    mEncodingFailed = false;
    mEncodingError.clear();
    resetStats();
    logInfo("Remote viewer disconnected.");
}

void RemoteStream::flushVideo()
{
    if (!mFrame.empty() && mVideo.write(mFrame.data(), mFrame.size(), mFrameOffset))
    {
        mFrame.clear();
        mFrameOffset = 0;
    }
}

void RemoteStream::pollCommands(const CommandHandler& onCommand)
{
    if (!mControl || mControlSession != mSession)
        return;
    if (!mReply.empty())
    {
        if (!mControl.write(mReply.data(), mReply.size(), mReplyOffset))
            return;
        mReply.clear();
        mReplyOffset = 0;
    }
    if (mCommand.empty())
    {
        if (!mControl.read(&mCommandSize, sizeof(mCommandSize), mCommandSizeOffset))
            return;
        const uint32_t size = ntohl(mCommandSize);
        if (!size || size > kMaxControlBytes)
        {
            closeControl();
            return;
        }
        mCommand.resize(size);
    }
    if (!mControl.read(mCommand.data(), mCommand.size(), mCommandOffset))
        return;

    json response = {{"id", nullptr}};
    try
    {
        const auto request = json::parse(mCommand);
        response["id"] = request.at("id");
        if (request.at("op") == "preview_encoding")
        {
            configureEncoding(request.at("changes"));
            response["result"] = encoding();
        }
        else
            response["result"] = onCommand(request);
    }
    catch (const std::exception& error)
    {
        const std::string message = error.what();
        response["error"] = message.substr(0, message.find("\n\nStacktrace:"));
    }
    std::string data = response.dump(-1, ' ', false, json::error_handler_t::replace);
    if (data.size() > kMaxControlBytes)
        data = json({{"id", response["id"]}, {"error", "The response exceeds the control message limit."}}).dump();
    const uint32_t size = htonl(uint32_t(data.size()));
    mReply.resize(sizeof(size) + data.size());
    std::memcpy(mReply.data(), &size, sizeof(size));
    std::memcpy(mReply.data() + sizeof(size), data.data(), data.size());
    mCommand.clear();
    mCommandSizeOffset = mCommandOffset = 0;
    if (mControl.write(mReply.data(), mReply.size(), mReplyOffset))
    {
        mReply.clear();
        mReplyOffset = 0;
    }
}

void RemoteStream::poll(const InputHandler& onInput, const CommandHandler& onCommand)
{
    if (mEncodingFailed)
    {
        mVideo.close();
        disconnect(onInput);
    }
    if (mVideo)
    {
        for (unsigned i = 0; i < 128 && mVideo.read(mInput.data(), sizeof(mInput), mInputOffset); ++i)
        {
            mInputOffset = 0;
            onInput(unpackInput(mInput));
        }
        if (mVideo)
            flushVideo();
        if (!mVideo)
            disconnect(onInput);
    }
    acceptPeers(onInput);
    if (mVideo)
        pollCommands(onCommand);
}

bool RemoteStream::ready() const
{
    return mVideo && mFrame.empty() && Clock::now() >= mNextFrame;
}

void RemoteStream::present(RenderContext& context, const Texture& texture)
{
    const auto now = Clock::now();
    const auto period = std::chrono::nanoseconds(1'000'000'000 / frameLimit());
    mNextFrame = std::max(mNextFrame + period, now);
    const auto active = mFramedVideo ? mEncoding : PreviewEncoding{PreviewCodec::H264, mMaxFps};
    const size_t prefix = (mFramedVideo ? 4 : 1) * sizeof(uint32_t);
    try
    {
        if (!mpEncoder)
            mpEncoder = std::make_unique<PreviewEncoder>(mpDevice, mPreviewSize, active);
        mFrame.resize(prefix);
        const auto timings = mpEncoder->encode(context, texture, mRequestIdr, mFrame);
        const size_t size = mFrame.size() - sizeof(uint32_t);
        FALCOR_CHECK(mFrame.size() > prefix && size <= kMaxFrameBytes, "Invalid preview frame size.");
        const uint32_t header[] = {
            htonl(uint32_t(size)), htonl(kPreviewMagic), htonl(uint32_t(active.codec)), htonl(mGeneration)
        };
        std::memcpy(mFrame.data(), header, prefix);
        mGpuWaitMs = timings.inputMs;
        mEncodeMs = timings.encodeMs;
    }
    catch (const std::exception& error)
    {
        mFrame.clear();
        mpEncoder.reset();
        mEncodingError = error.what();
        mEncodingError = mEncodingError.substr(0, mEncodingError.find("\n\nStacktrace:"));
        if (active.codec == PreviewCodec::H264)
            mEncodingFailed = true;
        else
        {
            mEncoding.codec = PreviewCodec::H264;
            mEncodingError += " Switched to H.264.";
            mRequestIdr = true;
            ++mGeneration;
            resetStats();
        }
        logError("Remote preview: {}", mEncodingError);
        return;
    }

    mRequestIdr = false;
    ++mEncodedFrames;
    mEncodedBytes += mFrame.size();
    const auto end = Clock::now();
    const auto elapsed = std::chrono::duration<double>(end - mFpsStart).count();
    if (elapsed >= 1.0)
    {
        mStreamFps = mEncodedFrames / elapsed;
        mStreamMbps = mEncodedBytes * 8.0 / elapsed / 1'000'000;
        mFpsStart = end;
        mEncodedFrames = 0;
        mEncodedBytes = 0;
    }
}
} // namespace Falcor::Remote
