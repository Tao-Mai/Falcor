#include "RemoteTransport.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <cerrno>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace Falcor::Remote
{
namespace
{
#ifdef _WIN32
using SocketLength = int;
constexpr int kInterrupted = WSAEINTR;
constexpr int kConnectionRefused = WSAECONNREFUSED;
constexpr int kTimedOut = WSAETIMEDOUT;
int lastSocketError() { return WSAGetLastError(); }
bool wouldBlock(int error) { return error == WSAEWOULDBLOCK; }
bool connecting(int error) { return wouldBlock(error); }

struct Winsock
{
    Winsock()
    {
        WSADATA data;
        const int status = WSAStartup(MAKEWORD(2, 2), &data);
        if (status)
            throw std::system_error(status, std::system_category(), "Initializing Winsock");
    }
    ~Winsock() { WSACleanup(); }
};
#else
using SocketLength = socklen_t;
constexpr int kInterrupted = EINTR;
constexpr int kConnectionRefused = ECONNREFUSED;
constexpr int kTimedOut = ETIMEDOUT;
int lastSocketError() { return errno; }
bool wouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
bool connecting(int error) { return error == EINPROGRESS; }
#endif

[[noreturn]] void socketError(const char* operation)
{
    throw std::system_error(lastSocketError(), std::system_category(), operation);
}
} // namespace

InputPacket packInput(const InputEvent& event)
{
    uint32_t x, y;
    static_assert(sizeof(float) == sizeof(uint32_t));
    std::memcpy(&x, &event.x, sizeof(x));
    std::memcpy(&y, &event.y, sizeof(y));
    return {htonl(uint32_t(event.type)), htonl(event.code), htonl(event.action), htonl(event.mods), htonl(x), htonl(y)};
}

InputEvent unpackInput(const InputPacket& packet)
{
    InputEvent event{InputType(ntohl(packet[0])), ntohl(packet[1]), ntohl(packet[2]), ntohl(packet[3])};
    const uint32_t x = ntohl(packet[4]);
    const uint32_t y = ntohl(packet[5]);
    std::memcpy(&event.x, &x, sizeof(x));
    std::memcpy(&event.y, &y, sizeof(y));
    return event;
}

Socket::~Socket()
{
    close();
}

Socket::Socket(Socket&& other) noexcept : mFd(std::exchange(other.mFd, kInvalid)) {}

Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        close();
        mFd = std::exchange(other.mFd, kInvalid);
    }
    return *this;
}

void Socket::close()
{
    if (mFd != kInvalid)
#ifdef _WIN32
        closesocket(std::exchange(mFd, kInvalid));
#else
        ::close(std::exchange(mFd, kInvalid));
#endif
}

#ifndef _WIN32
Socket Socket::listen(const char* address, uint16_t port)
{
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &endpoint.sin_addr) != 1)
        throw std::invalid_argument("--remote-bind requires an IPv4 address");

    Socket listener(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listener)
        socketError("Creating remote listener");
    const int enabled = 1;
    if (setsockopt(listener.mFd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) != 0)
        socketError("Configuring remote listener");
    if (::bind(listener.mFd, reinterpret_cast<const sockaddr*>(&endpoint), sizeof(endpoint)) != 0)
        socketError("Binding remote listener");
    if (::listen(listener.mFd, 1) != 0)
        socketError("Listening for remote viewer");
    return listener;
}

Socket Socket::accept() const
{
    Socket peer(::accept4(mFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
    if (peer)
        peer.configureConnection();
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        socketError("Accepting remote viewer");
    return peer;
}
#endif

Socket Socket::connect(const char* host, uint16_t port)
{
#ifdef _WIN32
    static const Winsock winsock;
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const int status = getaddrinfo(host, std::to_string(port).c_str(), &hints, &results);
    if (status != 0)
        throw std::runtime_error(gai_strerror(status));
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(results, freeaddrinfo);

    int failure = kConnectionRefused;
    for (const auto* address = results; address; address = address->ai_next)
    {
#ifdef _WIN32
        Socket peer(::socket(address->ai_family, SOCK_STREAM, 0));
#else
        Socket peer(::socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
#endif
        if (!peer)
        {
            failure = lastSocketError();
            continue;
        }
#ifdef _WIN32
        u_long nonblocking = 1;
        if (ioctlsocket(peer.mFd, FIONBIO, &nonblocking) != 0)
            socketError("Setting nonblocking mode");
#endif
        if (::connect(peer.mFd, address->ai_addr, SocketLength(address->ai_addrlen)) == 0)
        {
            peer.configureConnection();
            return peer;
        }
        failure = lastSocketError();
        if (!connecting(failure))
            continue;

        pollfd descriptor{peer.mFd, POLLOUT, 0};
#ifdef _WIN32
        const int ready = WSAPoll(&descriptor, 1, 5000);
#else
        const int ready = poll(&descriptor, 1, 5000);
#endif
        if (ready <= 0)
        {
            failure = ready == 0 ? kTimedOut : lastSocketError();
            continue;
        }
        SocketLength size = sizeof(failure);
        if (getsockopt(peer.mFd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&failure), &size) != 0)
            socketError("Checking the connection to Mogwai");
        if (failure == 0)
        {
            peer.configureConnection();
            return peer;
        }
    }
    throw std::system_error(failure, std::system_category(), "Connecting to Mogwai");
}

void Socket::configureConnection()
{
    const int enabled = 1;
    const int sendBytes = 64 * 1024;
    if (setsockopt(mFd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0 ||
        setsockopt(mFd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBytes), sizeof(sendBytes)) != 0)
        socketError("Configuring preview connection");
}

bool Socket::read(void* data, size_t size, size_t& offset)
{
    while (offset < size)
    {
        const auto count = recv(mFd, static_cast<char*>(data) + offset, int(size - offset), 0);
        if (count > 0)
            offset += size_t(count);
        else
        {
            const int error = lastSocketError();
            if (count < 0 && error == kInterrupted)
                continue;
            if (count == 0 || !wouldBlock(error))
                close();
            return false;
        }
    }
    return true;
}

bool Socket::write(const void* data, size_t size, size_t& offset)
{
    while (offset < size)
    {
#ifdef _WIN32
        constexpr int flags = 0;
#else
        constexpr int flags = MSG_NOSIGNAL;
#endif
        const auto count = send(mFd, static_cast<const char*>(data) + offset, int(size - offset), flags);
        if (count > 0)
            offset += size_t(count);
        else
        {
            const int error = lastSocketError();
            if (count < 0 && error == kInterrupted)
                continue;
            if (count == 0 || !wouldBlock(error))
                close();
            return false;
        }
    }
    return true;
}
} // namespace Falcor::Remote
