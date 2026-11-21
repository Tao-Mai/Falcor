#include "RemoteTransport.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/gl.h>
#include <ffnvcodec/dynlink_loader.h>
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace Falcor::Remote;

class Decoder
{
public:
    Decoder()
    {
        try
        {
            if (cuda_load_functions(&mCuda, nullptr) || cuvid_load_functions(&mCuvid, nullptr))
                throw std::runtime_error("Could not load CUDA and NVDEC from the NVIDIA driver");
            check(mCuda->cuInit(0), "Initializing CUDA");
            CUdevice device;
            check(mCuda->cuDeviceGet(&device, 0), "Finding the NVIDIA GPU");
            check(mCuda->cuCtxCreate(&mContext, 0, device), "Creating the CUDA context");

            CUVIDPARSERPARAMS params{};
            params.CodecType = cudaVideoCodec_H264;
            params.ulMaxNumDecodeSurfaces = 1;
            params.ulMaxDisplayDelay = 0;
            params.pUserData = this;
            params.pfnSequenceCallback = sequence;
            params.pfnDecodePicture = decodePicture;
            params.pfnDisplayPicture = displayPicture;
            check(mCuvid->cuvidCreateVideoParser(&mParser, &params), "Creating the H.264 parser");
        }
        catch (...)
        {
            release();
            throw;
        }
    }

    ~Decoder() { release(); }

    bool decode(const std::vector<uint8_t>& data)
    {
        mReady = false;
        CUVIDSOURCEDATAPACKET packet{};
        packet.flags = CUVID_PKT_ENDOFPICTURE;
        packet.payload_size = data.size();
        packet.payload = data.data();
        const CUresult result = mCuvid->cuvidParseVideoData(mParser, &packet);
        if (mError)
            std::rethrow_exception(mError);
        check(result, "Parsing an H.264 frame");
        return mReady;
    }

    int width() const { return mWidth; }
    int height() const { return mHeight; }
    const uint8_t* yPlane() const { return mPixels.data(); }
    const uint8_t* uvPlane() const { return mPixels.data() + size_t(mWidth) * mHeight; }

private:
    void check(CUresult result, const char* operation) const
    {
        if (result != CUDA_SUCCESS)
        {
            const char* name = nullptr;
            mCuda->cuGetErrorName(result, &name);
            throw std::runtime_error(std::string(operation) + ": " + (name ? name : "CUDA error"));
        }
    }

    void release()
    {
        if (mParser)
            mCuvid->cuvidDestroyVideoParser(mParser);
        if (mDecoder)
            mCuvid->cuvidDestroyDecoder(mDecoder);
        if (mContext)
            mCuda->cuCtxDestroy(mContext);
        cuvid_free_functions(&mCuvid);
        cuda_free_functions(&mCuda);
    }

    // CUVID calls these on the thread that is parsing the bitstream.
    template<typename Fn>
    int callback(Fn&& fn)
    {
        try
        {
            return fn();
        }
        catch (...)
        {
            mError = std::current_exception();
            return 0;
        }
    }

    static int CUDAAPI sequence(void* user, CUVIDEOFORMAT* format)
    {
        auto& self = *static_cast<Decoder*>(user);
        return self.callback([&]() {
            const auto& area = format->display_area;
            const int width = area.right - area.left;
            const int height = area.bottom - area.top;
            if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || width % 2 || height % 2 ||
                format->chroma_format != cudaVideoChromaFormat_420 || format->bit_depth_luma_minus8 != 0)
                throw std::runtime_error("Unsupported preview format. Expected 8-bit H.264 4:2:0.");

            if (self.mDecoder)
            {
                self.check(self.mCuvid->cuvidDestroyDecoder(self.mDecoder), "Releasing the previous decoder");
                self.mDecoder = nullptr;
            }
            CUVIDDECODECREATEINFO desc{};
            desc.CodecType = format->codec;
            desc.ChromaFormat = format->chroma_format;
            desc.OutputFormat = cudaVideoSurfaceFormat_NV12;
            desc.DeinterlaceMode = cudaVideoDeinterlaceMode_Weave;
            desc.ulWidth = format->coded_width;
            desc.ulHeight = format->coded_height;
            desc.display_area.left = area.left;
            desc.display_area.top = area.top;
            desc.display_area.right = area.right;
            desc.display_area.bottom = area.bottom;
            desc.ulTargetWidth = width;
            desc.ulTargetHeight = height;
            desc.ulNumDecodeSurfaces = format->min_num_decode_surfaces;
            desc.ulNumOutputSurfaces = 1;
            self.check(self.mCuvid->cuvidCreateDecoder(&self.mDecoder, &desc), "Starting NVDEC");
            self.mWidth = width;
            self.mHeight = height;
            self.mPixels.resize(size_t(width) * height * 3 / 2);
            return int(desc.ulNumDecodeSurfaces);
        });
    }

    static int CUDAAPI decodePicture(void* user, CUVIDPICPARAMS* params)
    {
        auto& self = *static_cast<Decoder*>(user);
        return self.callback([&]() {
            self.check(self.mCuvid->cuvidDecodePicture(self.mDecoder, params), "Decoding a preview frame");
            return 1;
        });
    }

    static int CUDAAPI displayPicture(void* user, CUVIDPARSERDISPINFO* info)
    {
        auto& self = *static_cast<Decoder*>(user);
        return self.callback([&]() {
            CUVIDPROCPARAMS params{};
            params.progressive_frame = info->progressive_frame;
            params.top_field_first = info->top_field_first;
            params.unpaired_field = info->repeat_first_field < 0;
            CUdeviceptr frame = 0;
            unsigned int pitch = 0;
            self.check(self.mCuvid->cuvidMapVideoFrame(self.mDecoder, info->picture_index, &frame, &pitch, &params), "Mapping the decoded frame");

            CUDA_MEMCPY2D copy{};
            copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
            copy.srcDevice = frame;
            copy.srcPitch = pitch;
            copy.dstMemoryType = CU_MEMORYTYPE_HOST;
            copy.dstHost = self.mPixels.data();
            copy.dstPitch = self.mWidth;
            copy.WidthInBytes = self.mWidth;
            copy.Height = self.mHeight * 3 / 2;
            const CUresult copied = self.mCuda->cuMemcpy2D(&copy);
            const CUresult unmapped = self.mCuvid->cuvidUnmapVideoFrame(self.mDecoder, frame);
            self.check(copied, "Copying the decoded frame");
            self.check(unmapped, "Releasing the decoded frame");
            self.mReady = true;
            return 1;
        });
    }

    CudaFunctions* mCuda = nullptr;
    CuvidFunctions* mCuvid = nullptr;
    CUcontext mContext = nullptr;
    CUvideoparser mParser = nullptr;
    CUvideodecoder mDecoder = nullptr;
    std::exception_ptr mError;
    std::vector<uint8_t> mPixels;
    int mWidth = 0, mHeight = 0;
    bool mReady = false;
};

struct Viewport
{
    int left, top, width, height;
};

Viewport fitVideo(int width, int height, int videoWidth, int videoHeight)
{
    const float scale = std::min(float(width) / videoWidth, float(height) / videoHeight);
    const int fittedWidth = std::max(1, int(scale * videoWidth));
    const int fittedHeight = std::max(1, int(scale * videoHeight));
    return {(width - fittedWidth) / 2, (height - fittedHeight) / 2, fittedWidth, fittedHeight};
}

class VideoDisplay
{
public:
    VideoDisplay()
    {
        const char* fragmentSource = R"glsl(#version 120
            uniform sampler2D luma;
            uniform sampler2D chroma;
            void main() {
                vec2 pos = gl_TexCoord[0].st;
                float y = (texture2D(luma, pos).r * 255.0 - 16.0) / 219.0;
                vec2 uv = (texture2D(chroma, pos).ra * 255.0 - 128.0) / 224.0;
                gl_FragColor = vec4(y + 1.5748 * uv.y,
                                    y - 0.1873 * uv.x - 0.4681 * uv.y,
                                    y + 1.8556 * uv.x, 1.0);
            }
        )glsl";
        const GLuint fragment = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragment, 1, &fragmentSource, nullptr);
        glCompileShader(fragment);
        GLint compiled = GL_FALSE;
        glGetShaderiv(fragment, GL_COMPILE_STATUS, &compiled);
        if (!compiled)
        {
            char message[512]{};
            glGetShaderInfoLog(fragment, sizeof(message), nullptr, message);
            glDeleteShader(fragment);
            throw std::runtime_error(std::string("Compiling the preview shader: ") + message);
        }

        mProgram = glCreateProgram();
        glAttachShader(mProgram, fragment);
        glLinkProgram(mProgram);
        glDeleteShader(fragment);
        GLint linked = GL_FALSE;
        glGetProgramiv(mProgram, GL_LINK_STATUS, &linked);
        if (!linked)
        {
            glDeleteProgram(mProgram);
            throw std::runtime_error("Linking the preview shader failed");
        }
        glUseProgram(mProgram);
        glUniform1i(glGetUniformLocation(mProgram, "luma"), 0);
        glUniform1i(glGetUniformLocation(mProgram, "chroma"), 1);
        glGenTextures(2, mTextures);
        for (GLuint texture : mTextures)
        {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }

    ~VideoDisplay()
    {
        glDeleteTextures(2, mTextures);
        glDeleteProgram(mProgram);
    }

    void update(const Decoder& decoder)
    {
        const bool resize = width != decoder.width() || height != decoder.height();
        width = decoder.width();
        height = decoder.height();
        upload(0, width, height, GL_LUMINANCE, decoder.yPlane(), resize);
        upload(1, width / 2, height / 2, GL_LUMINANCE_ALPHA, decoder.uvPlane(), resize);
    }

    void draw(int frameWidth, int frameHeight) const
    {
        glClear(GL_COLOR_BUFFER_BIT);
        if (width == 0 || frameWidth == 0 || frameHeight == 0)
            return;
        const auto area = fitVideo(frameWidth, frameHeight, width, height);
        glViewport(area.left, frameHeight - area.top - area.height, area.width, area.height);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0);
        glVertex2f(-1, 1);
        glTexCoord2f(1, 0);
        glVertex2f(1, 1);
        glTexCoord2f(1, 1);
        glVertex2f(1, -1);
        glTexCoord2f(0, 1);
        glVertex2f(-1, -1);
        glEnd();
    }

    int width = 0, height = 0;

private:
    void upload(unsigned int index, int w, int h, GLenum format, const uint8_t* data, bool resize)
    {
        glActiveTexture(GL_TEXTURE0 + index);
        glBindTexture(GL_TEXTURE_2D, mTextures[index]);
        if (resize)
            glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, format, GL_UNSIGNED_BYTE, data);
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, GL_UNSIGNED_BYTE, data);
    }

    GLuint mProgram = 0;
    GLuint mTextures[2]{};
};

struct Viewer
{
    Viewer(GLFWwindow* window, Socket socket) : window(window), socket(std::move(socket))
    {
        glfwSetWindowUserPointer(window, this);
        glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int, int action, int mods) {
            if (key != GLFW_KEY_UNKNOWN)
                from(w).enqueue({InputType::Key, uint32_t(key), uint32_t(action), uint32_t(mods)});
        });
        glfwSetCursorPosCallback(window, [](GLFWwindow* w, double x, double y) {
            from(w).pointer({InputType::Move}, x, y);
        });
        glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int mods) {
            double x, y;
            glfwGetCursorPos(w, &x, &y);
            from(w).pointer({InputType::Button, uint32_t(button), uint32_t(action), uint32_t(mods)}, x, y);
        });
        glfwSetScrollCallback(window, [](GLFWwindow* w, double x, double y) {
            from(w).enqueue({InputType::Scroll, 0, 0, 0, float(x), float(y)});
        });
        glfwSetWindowFocusCallback(window, [](GLFWwindow* w, int focused) {
            if (!focused)
                from(w).enqueue({InputType::ReleaseAll});
        });
        glfwSetWindowRefreshCallback(window, [](GLFWwindow* w) { from(w).redraw = true; });
    }

    void run()
    {
        Decoder decoder;
        uint32_t header = 0;
        size_t headerOffset = 0, frameOffset = 0;
        std::vector<uint8_t> frame;
        while (socket && !glfwWindowShouldClose(window))
        {
            glfwPollEvents();
            while (!input.empty() && socket.write(input.front().data(), sizeof(InputPacket), inputOffset))
            {
                input.pop_front();
                inputOffset = 0;
            }

            // Give the window and keyboard a turn even if several frames are already in the socket.
            bool decoded = false;
            for (int i = 0; socket && i < 4; ++i)
            {
                if (frame.empty())
                {
                    if (!socket.read(&header, sizeof(header), headerOffset))
                        break;
                    const uint32_t size = ntohl(header);
                    if (size == 0 || size > kMaxFrameBytes)
                        throw std::runtime_error("Invalid H.264 packet size from Mogwai");
                    frame.resize(size);
                    headerOffset = 0;
                }
                if (!socket.read(frame.data(), frame.size(), frameOffset))
                    break;
                decoded |= decoder.decode(frame);
                frame.clear();
                frameOffset = 0;
            }
            if (decoded)
            {
                if (display.width == 0)
                    std::fprintf(stderr, "Receiving %dx%d H.264 video (NVDEC).\n", decoder.width(), decoder.height());
                display.update(decoder);
                redraw = true;
            }
            if (redraw)
            {
                int width, height;
                glfwGetFramebufferSize(window, &width, &height);
                display.draw(width, height);
                glfwSwapBuffers(window);
                redraw = false;
            }
            else
                glfwWaitEventsTimeout(0.002);
        }
        if (!glfwWindowShouldClose(window))
            throw std::runtime_error("The connection to Mogwai closed");
    }

private:
    static Viewer& from(GLFWwindow* window) { return *static_cast<Viewer*>(glfwGetWindowUserPointer(window)); }

    void enqueue(const InputEvent& event)
    {
        const auto packet = packInput(event);
        if (event.type == InputType::Move && (input.size() > 1 || (!input.empty() && inputOffset == 0)) && input.back()[0] == packet[0])
            input.back() = packet;
        else if (input.size() < 256)
            input.push_back(packet);
        else
            socket.close();
    }

    void pointer(InputEvent event, double x, double y)
    {
        int width, height;
        glfwGetWindowSize(window, &width, &height);
        const auto area = display.width ? fitVideo(width, height, display.width, display.height) : Viewport{0, 0, width, height};
        if (area.width <= 0 || area.height <= 0)
            return;
        event.x = float((x - area.left) / area.width);
        event.y = float((y - area.top) / area.height);
        enqueue(event);
    }

    GLFWwindow* window;
    Socket socket;
    VideoDisplay display;
    std::deque<InputPacket> input;
    size_t inputOffset = 0;
    bool redraw = true;
};
} // namespace

int main(int argc, char** argv)
{
    unsigned int port = 0;
    if (argc == 3)
    {
        const char* end = argv[2] + std::strlen(argv[2]);
        const auto parsed = std::from_chars(argv[2], end, port);
        if (parsed.ec != std::errc{} || parsed.ptr != end)
            port = 0;
    }
    if (port == 0 || port > 65535)
    {
        std::fprintf(stderr, "Usage: FalcorRemoteViewer <host> <port>\n");
        return 1;
    }
    try
    {
        auto socket = Socket::connect(argv[1], uint16_t(port));
        glfwSetErrorCallback([](int, const char* message) { std::fprintf(stderr, "%s\n", message); });
        if (!glfwInit())
            throw std::runtime_error("Could not initialize the viewer window");
        const auto terminate = std::unique_ptr<void, void(*)(void*)>(reinterpret_cast<void*>(1), [](void*) { glfwTerminate(); });
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
        const std::unique_ptr<GLFWwindow, decltype(&glfwDestroyWindow)> window(
            glfwCreateWindow(1280, 720, "Falcor Remote Viewer", nullptr, nullptr), glfwDestroyWindow
        );
        if (!window)
            throw std::runtime_error("Could not open the viewer window");
        glfwMakeContextCurrent(window.get());
        if (!gladLoadGL(glfwGetProcAddress) || !GLAD_GL_VERSION_2_1)
            throw std::runtime_error("The preview window requires OpenGL 2.1");
        glfwSwapInterval(0);
        Viewer(window.get(), std::move(socket)).run();
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FalcorRemoteViewer: %s\n", e.what());
        return 1;
    }
}
