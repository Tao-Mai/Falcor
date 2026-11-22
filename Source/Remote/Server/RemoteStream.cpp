#include "RemoteStream.h"
#include "Core/API/GFXAPI.h"
#include "Core/API/LowLevelContextData.h"
#include "Utils/CudaUtils.h"
#include <nlohmann/json.hpp>
#include <ffnvcodec/nvEncodeAPI.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <algorithm>
#include <cstring>

namespace Falcor::Remote
{
using nlohmann::json;

namespace
{
constexpr uint32_t kBitrate = 15'000'000;

void checkEncode(NVENCSTATUS status, const char* operation)
{
    FALCOR_CHECK(status == NV_ENC_SUCCESS, "{} failed (NVENC status {}).", operation, int(status));
}

double milliseconds(std::chrono::steady_clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}
} // namespace

struct RemoteStream::Encoder
{
    Encoder(ref<Device> device, uint32_t width, uint32_t height, uint32_t fps) : width(width), height(height)
    {
        const auto cuda = device->getCudaDevice();
        library = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
        FALCOR_CHECK(library, "Could not load the NVIDIA NVENC driver: {}", dlerror());
        try
        {
            auto create = reinterpret_cast<decltype(&NvEncodeAPICreateInstance)>(dlsym(library, "NvEncodeAPICreateInstance"));
            FALCOR_CHECK(create, "The NVIDIA driver does not export NvEncodeAPICreateInstance.");
            api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            checkEncode(create(&api), "Loading NVENC");

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionDesc{};
            sessionDesc.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
            sessionDesc.apiVersion = NVENCAPI_VERSION;
            sessionDesc.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
            sessionDesc.device = cuda->getContext();
            checkEncode(api.nvEncOpenEncodeSessionEx(&sessionDesc, &session), "Opening the NVENC session");

            NV_ENC_PRESET_CONFIG preset{};
            preset.version = NV_ENC_PRESET_CONFIG_VER;
            preset.presetCfg.version = NV_ENC_CONFIG_VER;
            checkEncode(
                api.nvEncGetEncodePresetConfigEx(
                    session, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P2_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset
                ),
                "Choosing the H.264 encoding preset"
            );

            auto& config = preset.presetCfg;
            // TCP delivers every frame in order. Request an IDR when a viewer connects.
            config.gopLength = NVENC_INFINITE_GOPLENGTH;
            config.frameIntervalP = 1;
            config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            config.rcParams.averageBitRate = kBitrate;
            config.rcParams.maxBitRate = kBitrate;
            config.rcParams.vbvBufferSize = kBitrate / fps;
            config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
            config.rcParams.enableLookahead = 0;
            config.rcParams.zeroReorderDelay = 1;
            auto& h264 = config.encodeCodecConfig.h264Config;
            h264.idrPeriod = config.gopLength;
            h264.repeatSPSPPS = 1;
            auto& color = h264.h264VUIParameters;
            color.videoSignalTypePresentFlag = 1;
            color.videoFullRangeFlag = 0;
            color.colourDescriptionPresentFlag = 1;
            color.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            color.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            color.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;

            NV_ENC_INITIALIZE_PARAMS init{};
            init.version = NV_ENC_INITIALIZE_PARAMS_VER;
            init.encodeGUID = NV_ENC_CODEC_H264_GUID;
            init.presetGUID = NV_ENC_PRESET_P2_GUID;
            init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
            init.encodeWidth = init.darWidth = width;
            init.encodeHeight = init.darHeight = height;
            init.frameRateNum = fps;
            init.frameRateDen = 1;
            init.enablePTD = 1;
            init.encodeConfig = &config;
            checkEncode(api.nvEncInitializeEncoder(session, &init), "Starting H.264 encoding");


            size_t alignment = 1;
            FALCOR_GFX_CALL(device->getGfxDevice()->getTextureRowAlignment(&alignment));
            pitch = align_to(uint32_t(alignment), width * 4);
            buffer = device->createBuffer(
                size_t(pitch) * height, ResourceBindFlags::Shared, MemoryType::DeviceLocal, nullptr
            );
            memory = cuda_utils::importExternalMemory(buffer.get());
            cudaPointer = cuda_utils::externalMemoryGetMappedBuffer(memory, 0, buffer->getSize());

            NV_ENC_REGISTER_RESOURCE resource{};
            resource.version = NV_ENC_REGISTER_RESOURCE_VER;
            resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
            resource.resourceToRegister = cudaPointer;
            resource.width = width;
            resource.height = height;
            resource.pitch = pitch;
            resource.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
            resource.bufferUsage = NV_ENC_INPUT_IMAGE;
            checkEncode(api.nvEncRegisterResource(session, &resource), "Registering the GPU video buffer");
            input = resource.registeredResource;

            NV_ENC_CREATE_BITSTREAM_BUFFER output{};
            output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            checkEncode(api.nvEncCreateBitstreamBuffer(session, &output), "Allocating the H.264 output");
            bitstream = output.bitstreamBuffer;
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    ~Encoder() { release(); }

    void copyFrame(RenderContext& context, const Texture& texture)
    {
        context.resourceBarrier(&texture, Resource::State::CopySource);
        context.resourceBarrier(buffer.get(), Resource::State::CopyDest);
        auto encoder = context.getLowLevelData()->getResourceCommandEncoder();
        gfx::SubresourceRange range{};
        range.layerCount = 1;
        range.mipLevelCount = 1;
        encoder->copyTextureToBuffer(
            buffer->getGfxBufferResource(), 0, buffer->getSize(), pitch, texture.getGfxTextureResource(),
            gfx::ResourceState::CopySource, range, gfx::ITextureResource::Offset3D(0, 0, 0),
            gfx::ITextureResource::Extents{gfx::GfxIndex(width), gfx::GfxIndex(height), 1}
        );
        context.setPendingCommands(true);
        // Vulkan writes a device-local buffer imported by CUDA. Wait for that
        // copy before NVENC reads it. No uncompressed frame crosses PCIe.
        context.submit(true);
    }

    void encode(bool keyFrame, std::vector<uint8_t>& packet)
    {
        NV_ENC_MAP_INPUT_RESOURCE mapped{};
        mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapped.registeredResource = input;
        checkEncode(api.nvEncMapInputResource(session, &mapped), "Mapping the GPU video buffer");
        try
        {
            NV_ENC_PIC_PARAMS frame{};
            frame.version = NV_ENC_PIC_PARAMS_VER;
            frame.inputBuffer = mapped.mappedResource;
            frame.outputBitstream = bitstream;
            frame.bufferFmt = mapped.mappedBufferFmt;
            frame.inputWidth = width;
            frame.inputHeight = height;
            frame.inputPitch = pitch;
            frame.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            if (keyFrame)
                frame.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            checkEncode(api.nvEncEncodePicture(session, &frame), "Encoding a preview frame");

            NV_ENC_LOCK_BITSTREAM output{};
            output.version = NV_ENC_LOCK_BITSTREAM_VER;
            output.outputBitstream = bitstream;
            checkEncode(api.nvEncLockBitstream(session, &output), "Reading the H.264 bitstream");
            try
            {
                FALCOR_CHECK(output.bitstreamSizeInBytes > 0 && output.bitstreamSizeInBytes <= kMaxFrameBytes, "Invalid frame size.");
                const uint32_t size = htonl(output.bitstreamSizeInBytes);
                packet.resize(sizeof(size) + output.bitstreamSizeInBytes);
                std::memcpy(packet.data(), &size, sizeof(size));
                std::memcpy(packet.data() + sizeof(size), output.bitstreamBufferPtr, output.bitstreamSizeInBytes);
            }
            catch (...)
            {
                api.nvEncUnlockBitstream(session, bitstream);
                throw;
            }
            checkEncode(api.nvEncUnlockBitstream(session, bitstream), "Unlocking the H.264 bitstream");
        }
        catch (...)
        {
            api.nvEncUnmapInputResource(session, mapped.mappedResource);
            throw;
        }
        checkEncode(api.nvEncUnmapInputResource(session, mapped.mappedResource), "Unmapping the GPU video buffer");
    }

    const uint32_t width, height;

private:
    void release()
    {
        if (bitstream)
            api.nvEncDestroyBitstreamBuffer(session, bitstream);
        if (input)
            api.nvEncUnregisterResource(session, input);
        if (session)
            api.nvEncDestroyEncoder(session);
        if (cudaPointer)
            cudaFree(cudaPointer);
        if (memory)
            cudaDestroyExternalMemory(memory);
        dlclose(library);
    }

    uint32_t pitch = 0;
    ref<Buffer> buffer;
    cudaExternalMemory_t memory = nullptr;
    void* cudaPointer = nullptr;
    void* library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* session = nullptr;
    NV_ENC_REGISTERED_PTR input = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;
};

RemoteStream::RemoteStream(ref<Device> device, uint16_t port, uint32_t fps)
    : mpDevice(std::move(device)), mListener(Socket::listen("127.0.0.1", port)), mFps(fps)
{
    logInfo("Falcor Remote listening on 127.0.0.1:{} (video limit {} FPS).", port, fps);
}

RemoteStream::~RemoteStream() = default;

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
            mRequestIdr = true;
            mFpsStart = mNextFrame = Clock::now();
            mEncodedFrames = 0;
            mStreamFps = 0;
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
    mStreamFps = 0;
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
    const auto period = std::chrono::nanoseconds(1'000'000'000 / mFps);
    mNextFrame = std::max(mNextFrame + period, now);
    if (!mpEncoder)
        mpEncoder = std::make_unique<Encoder>(mpDevice, texture.getWidth(), texture.getHeight(), mFps);

    const auto copyStart = Clock::now();
    mpEncoder->copyFrame(context, texture);
    const auto encodeStart = Clock::now();
    mpEncoder->encode(mRequestIdr, mFrame);
    const auto end = Clock::now();
    mRequestIdr = false;
    mGpuWaitMs = milliseconds(encodeStart - copyStart);
    mEncodeMs = milliseconds(end - encodeStart);
    if (!mEncodedFrames && !mStreamFps)
        mFpsStart = end;
    ++mEncodedFrames;
    const auto elapsed = std::chrono::duration<double>(end - mFpsStart).count();
    if (elapsed >= 1.0)
    {
        mStreamFps = mEncodedFrames / elapsed;
        mFpsStart = end;
        mEncodedFrames = 0;
    }
}
} // namespace Falcor::Remote
