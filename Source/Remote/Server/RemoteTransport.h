#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace Falcor::Remote
{
inline constexpr uint32_t kMaxFrameBytes = 16 * 1024 * 1024;
inline constexpr uint32_t kMaxControlBytes = 1024 * 1024;

enum class InputType : uint32_t
{
    Key = 1,
    Move,
    Button,
    Scroll,
    ReleaseAll,
    Look,
    Text,
    Leave,
    EnableLookNavigation,
    OpenPreview = 11,
    OpenControls,
};

// The wire representation uses six big-endian words. Key, action and modifier
// codes use GLFW's values. Look uses press and release for capture and repeat
// for virtual cursor movement. Scroll carries wheel deltas.
struct InputEvent
{
    InputType type;
    uint32_t code = 0;
    uint32_t action = 0;
    uint32_t mods = 0;
    float x = 0.f;
    float y = 0.f;
};

using InputPacket = std::array<uint32_t, 6>;
InputPacket packInput(const InputEvent& event);
InputEvent unpackInput(const InputPacket& packet);

class Socket
{
public:
    Socket() = default;
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    static Socket connect(const char* host, uint16_t port);
#ifndef _WIN32
    static Socket listen(const char* address, uint16_t port);
    Socket accept(int timeoutMs) const;
    bool waitReadable(int timeoutMs) const;
#endif

    explicit operator bool() const { return mFd != kInvalid; }
    void close();

    // Continue a nonblocking transfer from offset. False means either the
    // socket would block or it has closed, which the caller can check with bool().
    bool read(void* data, size_t size, size_t& offset);
    bool write(const void* data, size_t size, size_t& offset);

private:
#ifdef _WIN32
    using Handle = SOCKET;
    static constexpr Handle kInvalid = INVALID_SOCKET;
#else
    using Handle = int;
    static constexpr Handle kInvalid = -1;
#endif
    explicit Socket(Handle fd) : mFd(fd) {}
    void configureConnection();
    Handle mFd = kInvalid;
};
} // namespace Falcor::Remote
