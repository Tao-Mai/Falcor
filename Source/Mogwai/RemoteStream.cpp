#include "RemoteStream.h"
#include "Mogwai.h"
#include "Core/Window.h"
#include "Utils/CudaUtils.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace Mogwai
{
using namespace Falcor;
using namespace Falcor::Remote;

namespace
{
constexpr uint32_t kFramesPerSecond = 60;
constexpr uint32_t kBitrate = 15'000'000;

void checkEncode(NVENCSTATUS status, const char* operation)
{
    FALCOR_CHECK(status == NV_ENC_SUCCESS, "{} failed (NVENC status {}).", operation, int(status));
}
} // namespace

struct RemoteStream::Encoder
{
    Encoder(CUcontext context, uint32_t width, uint32_t height) : width(width), height(height)
    {
        FALCOR_CHECK(width % 2 == 0 && height % 2 == 0 && width <= 4096 && height <= 4096, "Remote preview requires even dimensions up to 4096x4096.");
        library = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
        FALCOR_CHECK(library, "Could not load the NVIDIA NVENC driver: {}", dlerror());

        try
        {
            auto create = reinterpret_cast<decltype(&NvEncodeAPICreateInstance)>(dlsym(library, "NvEncodeAPICreateInstance"));
            FALCOR_CHECK(create, "The NVIDIA driver does not export NvEncodeAPICreateInstance.");
            api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            checkEncode(create(&api), "Loading the NVENC API");

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionDesc{};
            sessionDesc.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
            sessionDesc.apiVersion = NVENCAPI_VERSION;
            sessionDesc.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
            sessionDesc.device = context;
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
            config.gopLength = kFramesPerSecond;
            config.frameIntervalP = 1;
            config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
            config.rcParams.averageBitRate = kBitrate;
            config.rcParams.maxBitRate = kBitrate;
            config.rcParams.vbvBufferSize = kBitrate / kFramesPerSecond;
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
            init.frameRateNum = kFramesPerSecond;
            init.frameRateDen = 1;
            init.enablePTD = 1;
            init.encodeConfig = &config;
            checkEncode(api.nvEncInitializeEncoder(session, &init), "Starting H.264 encoding");

            NV_ENC_CREATE_INPUT_BUFFER source{};
            source.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
            source.width = width;
            source.height = height;
            // In little-endian memory NVENC's packed ARGB format is BGRA.
            source.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
            checkEncode(api.nvEncCreateInputBuffer(session, &source), "Allocating the encoder input");
            input = source.inputBuffer;

            NV_ENC_CREATE_BITSTREAM_BUFFER output{};
            output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            checkEncode(api.nvEncCreateBitstreamBuffer(session, &output), "Allocating the encoder bitstream");
            bitstream = output.bitstreamBuffer;
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    ~Encoder() { release(); }

    void encode(const std::vector<uint8_t>& pixels, bool keyFrame, std::vector<uint8_t>& packet)
    {
        NV_ENC_LOCK_INPUT_BUFFER source{};
        source.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        source.inputBuffer = input;
        checkEncode(api.nvEncLockInputBuffer(session, &source), "Locking the encoder input");
        const size_t rowBytes = size_t(width) * 4;
        auto* destination = static_cast<uint8_t*>(source.bufferDataPtr);
        for (uint32_t row = 0; row < height; ++row)
            std::memcpy(destination + row * source.pitch, pixels.data() + row * rowBytes, rowBytes);
        checkEncode(api.nvEncUnlockInputBuffer(session, input), "Unlocking the encoder input");

        NV_ENC_PIC_PARAMS frame{};
        frame.version = NV_ENC_PIC_PARAMS_VER;
        frame.inputBuffer = input;
        frame.outputBitstream = bitstream;
        frame.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
        frame.inputWidth = width;
        frame.inputHeight = height;
        frame.inputPitch = source.pitch;
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
            FALCOR_CHECK(output.bitstreamSizeInBytes > 0 && output.bitstreamSizeInBytes <= kMaxFrameBytes, "Invalid preview frame size.");
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

    const uint32_t width, height;

private:
    void release()
    {
        if (bitstream)
            api.nvEncDestroyBitstreamBuffer(session, bitstream);
        if (input)
            api.nvEncDestroyInputBuffer(session, input);
        if (session)
            api.nvEncDestroyEncoder(session);
        dlclose(library);
    }

    void* library = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api{};
    void* session = nullptr;
    NV_ENC_INPUT_PTR input = nullptr;
    NV_ENC_OUTPUT_PTR bitstream = nullptr;
};

RemoteStream::RemoteStream(Renderer& renderer, const std::string& bindAddress, uint16_t port)
    : mRenderer(renderer), mListener(Socket::listen(bindAddress.c_str(), port))
{
    logInfo("Remote preview listening on {}:{}.", bindAddress, port);
}

RemoteStream::~RemoteStream() = default;

void RemoteStream::pollInput(const Fbo& target)
{
    mSize = float2(float(target.getWidth()), float(target.getHeight()));
    if (!mClient)
    {
        mClient = mListener.accept();
        if (!mClient)
            return;
        logInfo("Remote viewer connected.");
    }

    for (unsigned int i = 0; i < 128 && mClient.read(mInput.data(), sizeof(mInput), mInputOffset); ++i)
    {
        mInputOffset = 0;
        handleInput(unpackInput(mInput));
    }

    if (mClient)
        flushFrame();
    else
        disconnect();
}

void RemoteStream::flushFrame()
{
    if (!mFrame.empty() && mClient.write(mFrame.data(), mFrame.size(), mFrameOffset))
    {
        mFrame.clear();
        mFrameOffset = 0;
    }
    if (!mClient)
        disconnect();
}

void RemoteStream::disconnect()
{
    releaseInput();
    mFrame.clear();
    mInputOffset = mFrameOffset = 0;
    mRequestIdr = true;
    logInfo("Remote viewer disconnected.");
}

void RemoteStream::sendFrame(RenderContext& context, const Texture& texture)
{
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    // Keep at most one partially sent access unit. Skip source frames while it
    // drains so the decoder always receives the frames that subsequent ones reference.
    if (!mClient || !mFrame.empty() || now < mNextFrame)
        return;
    const auto period = std::chrono::microseconds(1'000'000 / kFramesPerSecond);
    mNextFrame = std::max(mNextFrame + period, now);

    const auto format = texture.getFormat();
    FALCOR_CHECK(format == ResourceFormat::BGRA8UnormSrgb || format == ResourceFormat::BGRA8Unorm, "Remote preview requires a BGRA8 framebuffer.");
    const uint32_t width = texture.getWidth(), height = texture.getHeight();
    if (!mpEncoder || mpEncoder->width != width || mpEncoder->height != height)
    {
        mpEncoder.reset();
        mpEncoder = std::make_unique<Encoder>(mRenderer.getDevice()->getCudaDevice()->getContext(), width, height);
        mRequestIdr = true;
    }
    auto pixels = context.readTextureSubresource(&texture, texture.getSubresourceIndex(0, 0));
    mpEncoder->encode(pixels, mRequestIdr, mFrame);
    mRequestIdr = false;
    flushFrame();
}

void RemoteStream::handleInput(const InputEvent& input)
{
    if (input.type == InputType::ReleaseAll)
    {
        releaseInput();
        return;
    }
    if (input.type == InputType::Key)
    {
        KeyboardEvent event{};
        if (input.code <= GLFW_KEY_LAST && input.action <= GLFW_REPEAT &&
            Window::translateGlfwKeyEvent(int(input.code), int(input.action), int(input.mods & 7), event) &&
            event.key != Input::Key::Unknown)
        {
            mKeys.set(size_t(event.key), input.action != GLFW_RELEASE);
            mRenderer.onKeyEvent(event);
        }
        return;
    }

    if (!std::isfinite(input.x) || !std::isfinite(input.y))
        return;
    MouseEvent event{};
    if (input.type == InputType::Scroll)
    {
        event.type = MouseEvent::Type::Wheel;
        event.wheelDelta = float2(std::clamp(input.x, -20.f, 20.f), std::clamp(input.y, -20.f, 20.f));
    }
    else
    {
        mPointer = float2(std::clamp(input.x, 0.f, 1.f), std::clamp(input.y, 0.f, 1.f));
        if (input.type == InputType::Move)
            event.type = MouseEvent::Type::Move;
        else if (input.type == InputType::Button && input.code <= GLFW_MOUSE_BUTTON_MIDDLE && input.action <= GLFW_PRESS)
        {
            constexpr Input::MouseButton buttons[] = {Input::MouseButton::Left, Input::MouseButton::Right, Input::MouseButton::Middle};
            event.type = input.action == GLFW_PRESS ? MouseEvent::Type::ButtonDown : MouseEvent::Type::ButtonUp;
            event.button = buttons[input.code];
            event.mods = Input::ModifierFlags(input.mods & 7);
            mButtons.set(size_t(event.button), input.action == GLFW_PRESS);
        }
        else
            return;
    }
    event.pos = mPointer;
    event.screenPos = mPointer * mSize;
    mRenderer.onMouseEvent(event);
}

void RemoteStream::releaseInput()
{
    for (size_t key = 0; key < mKeys.size(); ++key)
    {
        if (mKeys[key])
            mRenderer.onKeyEvent({KeyboardEvent::Type::KeyReleased, Input::Key(key), Input::ModifierFlags::None});
    }
    for (size_t button = 0; button < mButtons.size(); ++button)
    {
        if (mButtons[button])
        {
            MouseEvent event{};
            event.type = MouseEvent::Type::ButtonUp;
            event.button = Input::MouseButton(button);
            event.pos = mPointer;
            event.screenPos = mPointer * mSize;
            mRenderer.onMouseEvent(event);
        }
    }
    mKeys.reset();
    mButtons.reset();
}
} // namespace Mogwai
