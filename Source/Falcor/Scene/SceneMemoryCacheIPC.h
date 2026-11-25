#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace Falcor::SceneMemoryCacheIPC
{
using Key = std::array<uint8_t, 20>;
constexpr uint32_t kProtocol = 1;
constexpr DWORD kIoTimeoutMs = 1500;
constexpr wchar_t kHostExecutable[] = L"MogwaiSceneCacheHost.exe";
constexpr wchar_t kPidFile[] = L"host.pid";

enum class Command : uint32_t { Lookup, Reserve, Publish, Erase, Configure, Clear };
enum class Result : uint32_t { Miss, Success, Rejected };

struct Request
{
    uint32_t protocol = kProtocol;
    Command command = Command::Lookup;
    Key key{};
    uint32_t ttlSeconds = 0;
    uint64_t limitBytes = 0;
    uint64_t bytes = 0;
    uint64_t id = 0;
};

struct Reply
{
    uint32_t protocol = kProtocol;
    uint32_t pid = 0;
    Result result = Result::Miss;
    uint32_t reserved = 0;
    uint64_t id = 0;
    uint64_t bytes = 0;
};

struct CloseHandle
{
    void operator()(HANDLE handle) const { ::CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandle>;

inline Handle ownHandle(HANDLE handle)
{
    return Handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
}

struct UnmapView
{
    void operator()(void* data) const { UnmapViewOfFile(data); }
};
using View = std::unique_ptr<void, UnmapView>;

inline std::wstring pipeName(uint32_t pid)
{
    return L"\\\\.\\pipe\\Falcor.MogwaiSceneCache.v1." + std::to_wstring(pid);
}

inline std::wstring mappingName(uint32_t pid, uint64_t id)
{
    return L"Local\\Falcor.MogwaiSceneCache.v1." + std::to_wstring(pid) + L"." + std::to_wstring(id);
}

inline bool waitForIo(HANDLE pipe, OVERLAPPED& operation, DWORD& bytes, DWORD timeoutMs)
{
    if (GetLastError() != ERROR_IO_PENDING)
        return false;

    if (WaitForSingleObject(operation.hEvent, timeoutMs) == WAIT_OBJECT_0)
        return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;

    // Wait for cancellation before releasing the OVERLAPPED and its I/O buffer.
    CancelIoEx(pipe, &operation);
    GetOverlappedResult(pipe, &operation, &bytes, TRUE);
    return false;
}

template<typename T>
bool transfer(HANDLE pipe, HANDLE event, T& value, bool write)
{
    ResetEvent(event);
    OVERLAPPED operation{};
    operation.hEvent = event;
    DWORD bytes = 0;
    constexpr DWORD size = static_cast<DWORD>(sizeof(T));
    const BOOL completed = write ? WriteFile(pipe, &value, size, &bytes, &operation) : ReadFile(pipe, &value, size, &bytes, &operation);
    return (completed || waitForIo(pipe, operation, bytes, kIoTimeoutMs)) && bytes == size;
}
} // namespace Falcor::SceneMemoryCacheIPC
