#include "PreviewEncoder.h"
#include "Core/API/GFXAPI.h"
#include "Core/API/LowLevelContextData.h"
#include "Core/Pass/ComputePass.h"
#include "Utils/CudaUtils.h"
#include <ffnvcodec/nvEncodeAPI.h>
#include <nvjpeg.h>
#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace Falcor::Remote
{
namespace
{
using Clock = std::chrono::steady_clock;

struct EncoderImage
{
    EncoderImage(const ref<Device>& device, uint2 size, bool planar)
        : size(size),
          bgraPitch(align_to(std::max(16u, uint32_t(device->getTextureRowAlignment())), size.x * 4)),
          pitch(planar ? align_to(256u, size.x) : bgraPitch)
    {
        bgra = device->createBuffer(
            size_t(bgraPitch) * size.y, planar ? ResourceBindFlags::ShaderResource : ResourceBindFlags::Shared,
            MemoryType::DeviceLocal
        );
        if (planar)
        {
            planes = device->createBuffer(
                size_t(pitch) * size.y * 3, ResourceBindFlags::Shared | ResourceBindFlags::UnorderedAccess, MemoryType::DeviceLocal
            );
            pack = ComputePass::create(device, "Remote/PackPreview.cs.slang", "main");
            auto root = pack->getRootVar();
            root["gBgra"] = bgra;
            root["gPlanes"] = planes;
            auto params = root["FrameParams"];
            params["gWidth"] = size.x;
            params["gHeight"] = size.y;
            params["gBgraPitch"] = bgraPitch;
            params["gPlanePitch"] = pitch;
        }

        const auto& shared = planar ? planes : bgra;
        memory = cuda_utils::importExternalMemory(shared.get());
        try
        {
            data = cuda_utils::externalMemoryGetMappedBuffer(memory, 0, shared->getSize());
        }
        catch (...)
        {
            cudaDestroyExternalMemory(memory);
            throw;
        }
    }

    ~EncoderImage()
    {
        cudaFree(data);
        cudaDestroyExternalMemory(memory);
    }

    void copy(RenderContext& context, const Texture& texture)
    {
        FALCOR_CHECK(
            texture.getWidth() == size.x && texture.getHeight() == size.y && texture.getFormat() == ResourceFormat::BGRA8UnormSrgb,
            "The encoder expects a {} x {} BGRA8 sRGB preview texture.", size.x, size.y
        );
        context.resourceBarrier(&texture, Resource::State::CopySource);
        context.resourceBarrier(bgra.get(), Resource::State::CopyDest);
        auto encoder = context.getLowLevelData()->getResourceCommandEncoder();
        gfx::SubresourceRange range{};
        range.layerCount = 1;
        range.mipLevelCount = 1;
        encoder->copyTextureToBuffer(
            bgra->getGfxBufferResource(), 0, bgra->getSize(), bgraPitch, texture.getGfxTextureResource(),
            gfx::ResourceState::CopySource, range, gfx::ITextureResource::Offset3D(0, 0, 0),
            gfx::ITextureResource::Extents{gfx::GfxIndex(size.x), gfx::GfxIndex(size.y), 1}
        );
        context.setPendingCommands(true);
        if (pack)
            pack->execute(&context, div_round_up(size.x, 4u), size.y);
        // CUDA reads the same device memory after the graphics queue finishes writing it.
        context.submit(true);
    }

    const uint2 size;
    const uint32_t bgraPitch, pitch;
    void* data = nullptr;
    ref<Buffer> bgra, planes;
    ref<ComputePass> pack;
    cudaExternalMemory_t memory = nullptr;
};

void checkNvenc(NVENCSTATUS status, const char* operation)
{
    FALCOR_CHECK(status == NV_ENC_SUCCESS, "{} failed (NVENC status {}).", operation, int(status));
}

void checkJpeg(nvjpegStatus_t status, const char* operation)
{
    FALCOR_CHECK(status == NVJPEG_STATUS_SUCCESS, "{} failed (nvJPEG status {}).", operation, int(status));
}

void setColorDescription(NV_ENC_CONFIG_H264_VUI_PARAMETERS& color, bool rgb)
{
    color.videoSignalTypePresentFlag = 1;
    color.videoFullRangeFlag = rgb;
    color.colourDescriptionPresentFlag = 1;
    color.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    color.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
    color.colourMatrix = rgb ? NV_ENC_VUI_MATRIX_COEFFS_RGB : NV_ENC_VUI_MATRIX_COEFFS_BT709;
}

class VideoEncoder
{
public:
    VideoEncoder(CUcontext cuda, const EncoderImage& image, const PreviewEncoding& settings)
    {
        mLibrary = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
        FALCOR_CHECK(mLibrary, "Could not load the NVIDIA NVENC driver: {}", dlerror());
        try
        {
            auto create = reinterpret_cast<decltype(&NvEncodeAPICreateInstance)>(dlsym(mLibrary, "NvEncodeAPICreateInstance"));
            FALCOR_CHECK(create, "The NVIDIA driver does not export NvEncodeAPICreateInstance.");
            mApi.version = NV_ENCODE_API_FUNCTION_LIST_VER;
            checkNvenc(create(&mApi), "Loading NVENC");

            NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS session{};
            session.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
            session.apiVersion = NVENCAPI_VERSION;
            session.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
            session.device = cuda;
            checkNvenc(mApi.nvEncOpenEncodeSessionEx(&session, &mSession), "Opening NVENC");

            const bool hevc = settings.codec != PreviewCodec::H264;
            const bool rgb = settings.codec == PreviewCodec::HevcRgb;
            const auto codec = hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID;
            const auto tune = rgb ? NV_ENC_TUNING_INFO_LOSSLESS : NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
            if (rgb)
            {
                for (auto capability : {NV_ENC_CAPS_SUPPORT_YUV444_ENCODE, NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE})
                {
                    NV_ENC_CAPS_PARAM caps{};
                    caps.version = NV_ENC_CAPS_PARAM_VER;
                    caps.capsToQuery = capability;
                    int supported = 0;
                    checkNvenc(mApi.nvEncGetEncodeCaps(mSession, codec, &caps, &supported), "Checking HEVC capabilities");
                    FALCOR_CHECK(supported, "The server GPU does not support HEVC lossless RGB encoding.");
                }
            }

            NV_ENC_PRESET_CONFIG preset{};
            preset.version = NV_ENC_PRESET_CONFIG_VER;
            preset.presetCfg.version = NV_ENC_CONFIG_VER;
            checkNvenc(mApi.nvEncGetEncodePresetConfigEx(mSession, codec, NV_ENC_PRESET_P2_GUID, tune, &preset), "Choosing an NVENC preset");
            auto& config = preset.presetCfg;
            // Connections and codec changes start with an IDR. TCP preserves reference-frame order afterwards.
            config.gopLength = NVENC_INFINITE_GOPLENGTH;
            config.frameIntervalP = 1;
            auto& rc = config.rcParams;
            rc.enableLookahead = 0;
            rc.zeroReorderDelay = 1;
            if (rgb)
            {
                rc.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
                rc.constQP = {0, 0, 0};
            }
            else
            {
                const uint32_t bitrate = settings.bitrateMbps * 1'000'000;
                rc.rateControlMode = NV_ENC_PARAMS_RC_CBR;
                rc.averageBitRate = rc.maxBitRate = bitrate;
                rc.vbvBufferSize = bitrate / settings.fps;
                rc.vbvInitialDelay = rc.vbvBufferSize;
            }

            if (hevc)
            {
                config.profileGUID = rgb ? NV_ENC_HEVC_PROFILE_FREXT_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID;
                auto& options = config.encodeCodecConfig.hevcConfig;
                options.chromaFormatIDC = rgb ? 3 : 1;
                options.inputBitDepth = options.outputBitDepth = NV_ENC_BIT_DEPTH_8;
                options.idrPeriod = config.gopLength;
                options.repeatSPSPPS = 1;
                setColorDescription(options.hevcVUIParameters, rgb);
            }
            else
            {
                auto& options = config.encodeCodecConfig.h264Config;
                options.idrPeriod = config.gopLength;
                options.repeatSPSPPS = 1;
                setColorDescription(options.h264VUIParameters, false);
            }

            NV_ENC_INITIALIZE_PARAMS init{};
            init.version = NV_ENC_INITIALIZE_PARAMS_VER;
            init.encodeGUID = codec;
            init.presetGUID = NV_ENC_PRESET_P2_GUID;
            init.tuningInfo = tune;
            init.encodeWidth = init.darWidth = image.size.x;
            init.encodeHeight = init.darHeight = image.size.y;
            init.frameRateNum = settings.fps;
            init.frameRateDen = 1;
            init.enablePTD = 1;
            init.encodeConfig = &config;
            checkNvenc(mApi.nvEncInitializeEncoder(mSession, &init), "Starting NVENC");

            NV_ENC_REGISTER_RESOURCE resource{};
            resource.version = NV_ENC_REGISTER_RESOURCE_VER;
            resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
            resource.resourceToRegister = image.data;
            resource.width = image.size.x;
            resource.height = image.size.y;
            resource.pitch = image.pitch;
            resource.bufferFormat = rgb ? NV_ENC_BUFFER_FORMAT_YUV444 : NV_ENC_BUFFER_FORMAT_ARGB;
            resource.bufferUsage = NV_ENC_INPUT_IMAGE;
            checkNvenc(mApi.nvEncRegisterResource(mSession, &resource), "Registering the NVENC input");
            mInput = resource.registeredResource;

            NV_ENC_CREATE_BITSTREAM_BUFFER output{};
            output.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            checkNvenc(mApi.nvEncCreateBitstreamBuffer(mSession, &output), "Allocating the NVENC bitstream");
            mBitstream = output.bitstreamBuffer;
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    ~VideoEncoder() { release(); }

    void append(const EncoderImage& image, bool keyFrame, std::vector<uint8_t>& packet)
    {
        NV_ENC_MAP_INPUT_RESOURCE mapped{};
        mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapped.registeredResource = mInput;
        checkNvenc(mApi.nvEncMapInputResource(mSession, &mapped), "Mapping the NVENC input");
        try
        {
            NV_ENC_PIC_PARAMS frame{};
            frame.version = NV_ENC_PIC_PARAMS_VER;
            frame.inputBuffer = mapped.mappedResource;
            frame.outputBitstream = mBitstream;
            frame.bufferFmt = mapped.mappedBufferFmt;
            frame.inputWidth = image.size.x;
            frame.inputHeight = image.size.y;
            frame.inputPitch = image.pitch;
            frame.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            if (keyFrame)
                frame.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
            checkNvenc(mApi.nvEncEncodePicture(mSession, &frame), "Encoding a preview frame");

            NV_ENC_LOCK_BITSTREAM output{};
            output.version = NV_ENC_LOCK_BITSTREAM_VER;
            output.outputBitstream = mBitstream;
            checkNvenc(mApi.nvEncLockBitstream(mSession, &output), "Reading the NVENC bitstream");
            try
            {
                const size_t offset = packet.size();
                packet.resize(offset + output.bitstreamSizeInBytes);
                if (output.bitstreamSizeInBytes)
                    std::memcpy(packet.data() + offset, output.bitstreamBufferPtr, output.bitstreamSizeInBytes);
            }
            catch (...)
            {
                mApi.nvEncUnlockBitstream(mSession, mBitstream);
                throw;
            }
            checkNvenc(mApi.nvEncUnlockBitstream(mSession, mBitstream), "Unlocking the NVENC bitstream");
        }
        catch (...)
        {
            mApi.nvEncUnmapInputResource(mSession, mapped.mappedResource);
            throw;
        }
        checkNvenc(mApi.nvEncUnmapInputResource(mSession, mapped.mappedResource), "Unmapping the NVENC input");
    }

private:
    void release()
    {
        if (mBitstream)
            mApi.nvEncDestroyBitstreamBuffer(mSession, mBitstream);
        if (mInput)
            mApi.nvEncUnregisterResource(mSession, mInput);
        if (mSession)
            mApi.nvEncDestroyEncoder(mSession);
        dlclose(mLibrary);
    }

    void* mLibrary = nullptr;
    NV_ENCODE_API_FUNCTION_LIST mApi{};
    void* mSession = nullptr;
    NV_ENC_REGISTERED_PTR mInput = nullptr;
    NV_ENC_OUTPUT_PTR mBitstream = nullptr;
};

class JpegEncoder
{
public:
    JpegEncoder(cudaStream_t stream, uint2 size, const PreviewEncoding& settings) : mStream(stream)
    {
        try
        {
            checkJpeg(nvjpegCreateSimple(&mHandle), "Opening nvJPEG");
            checkJpeg(nvjpegEncoderStateCreate(mHandle, &mState, mStream), "Creating the JPEG encoder");
            checkJpeg(nvjpegEncoderParamsCreate(mHandle, &mParameters, mStream), "Creating JPEG settings");
            checkJpeg(nvjpegEncoderParamsSetQuality(mParameters, settings.jpegQuality, mStream), "Setting JPEG quality");
            const auto sampling = settings.jpeg444 ? NVJPEG_CSS_444 : NVJPEG_CSS_420;
            checkJpeg(nvjpegEncoderParamsSetSamplingFactors(mParameters, sampling, mStream), "Setting JPEG chroma sampling");
            checkJpeg(nvjpegEncodeGetBufferSize(mHandle, mParameters, size.x, size.y, &mCapacity), "Sizing the JPEG bitstream");
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    ~JpegEncoder() { release(); }

    void append(const EncoderImage& image, std::vector<uint8_t>& packet)
    {
        const size_t planeSize = size_t(image.pitch) * image.size.y;
        auto* bytes = static_cast<uint8_t*>(image.data);
        nvjpegImage_t rgb{};
        rgb.channel[0] = bytes + 2 * planeSize;
        rgb.channel[1] = bytes;
        rgb.channel[2] = bytes + planeSize;
        rgb.pitch[0] = rgb.pitch[1] = rgb.pitch[2] = image.pitch;
        const size_t offset = packet.size();
        size_t size = mCapacity;
        packet.resize(offset + size);
        checkJpeg(
            nvjpegEncodeImage(mHandle, mState, mParameters, &rgb, NVJPEG_INPUT_RGB, image.size.x, image.size.y, mStream),
            "Encoding a JPEG frame"
        );
        checkJpeg(nvjpegEncodeRetrieveBitstream(mHandle, mState, packet.data() + offset, &size, mStream), "Reading the JPEG bitstream");
        FALCOR_CUDA_CHECK(cudaStreamSynchronize(mStream));
        packet.resize(offset + size);
    }

private:
    void release()
    {
        if (mParameters)
            nvjpegEncoderParamsDestroy(mParameters);
        if (mState)
            nvjpegEncoderStateDestroy(mState);
        if (mHandle)
            nvjpegDestroy(mHandle);
    }

    cudaStream_t mStream;
    nvjpegHandle_t mHandle = nullptr;
    nvjpegEncoderState_t mState = nullptr;
    nvjpegEncoderParams_t mParameters = nullptr;
    size_t mCapacity = 0;
};
} // namespace

struct PreviewEncoder::Impl
{
    Impl(const ref<Device>& device, uint2 size, const PreviewEncoding& settings)
        : cuda(device->getCudaDevice()),
          image(device, size, settings.codec == PreviewCodec::HevcRgb || settings.codec == PreviewCodec::Jpeg)
    {
        if (settings.codec == PreviewCodec::Jpeg)
            jpeg = std::make_unique<JpegEncoder>(cuda->getStream(), size, settings);
        else
            video = std::make_unique<VideoEncoder>(cuda->getContext(), image, settings);
    }

    ref<cuda_utils::CudaDevice> cuda;
    EncoderImage image;
    std::unique_ptr<VideoEncoder> video;
    std::unique_ptr<JpegEncoder> jpeg;
};

PreviewEncoder::PreviewEncoder(ref<Device> device, uint2 size, const PreviewEncoding& settings)
    : mpImpl(std::make_unique<Impl>(device, size, settings))
{}

PreviewEncoder::~PreviewEncoder() = default;

EncodeTimings PreviewEncoder::encode(RenderContext& context, const Texture& texture, bool keyFrame, std::vector<uint8_t>& output)
{
    const auto start = Clock::now();
    mpImpl->image.copy(context, texture);
    const auto encodeStart = Clock::now();
    if (mpImpl->video)
        mpImpl->video->append(mpImpl->image, keyFrame, output);
    else
        mpImpl->jpeg->append(mpImpl->image, output);
    const auto end = Clock::now();
    return {
        std::chrono::duration<double, std::milli>(encodeStart - start).count(),
        std::chrono::duration<double, std::milli>(end - encodeStart).count()
    };
}
} // namespace Falcor::Remote
