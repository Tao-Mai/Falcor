#include "Scene/SceneMemoryCacheIPC.h"

#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <vector>

namespace
{
using namespace Falcor::SceneMemoryCacheIPC;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct FreeLocal
{
    void operator()(void* pointer) const { LocalFree(pointer); }
};

std::unique_ptr<void, FreeLocal> createUserSecurity()
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        return {};
    auto token = ownHandle(rawToken);

    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (size == 0)
        return {};
    std::vector<uint8_t> user(size);
    if (!GetTokenInformation(token.get(), TokenUser, user.data(), size, &size))
        return {};

    LPWSTR rawSid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid, &rawSid))
        return {};
    const std::unique_ptr<wchar_t, FreeLocal> sid(rawSid);
    const auto descriptor = std::wstring(L"D:P(A;;GA;;;") + sid.get() + L")";
    PSECURITY_DESCRIPTOR security = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr))
        return {};

    return std::unique_ptr<void, FreeLocal>(security);
}

struct Entry
{
    Handle mapping;
    Handle writer;
    uint32_t writerPid = 0;
    uint64_t id = 0;
    uint64_t bytes = 0;
    Clock::time_point lastUse;
};

class Cache
{
public:
    explicit Cache(SECURITY_ATTRIBUTES& security) : mSecurity(security) {}

    bool empty() const { return mEntries.empty(); }

    void expire(Clock::time_point now)
    {
        for (auto it = mEntries.begin(); it != mEntries.end();)
        {
            const auto& entry = it->second;
            const bool expired = entry.writer
                ? WaitForSingleObject(entry.writer.get(), 0) == WAIT_OBJECT_0
                : now - entry.lastUse >= mTtl;
            it = expired ? mEntries.erase(it) : std::next(it);
        }
    }

    Reply apply(const Request& request, uint32_t clientPid, Clock::time_point now)
    {
        Reply reply;
        reply.pid = mPid;
        if (request.command == Command::Lookup || request.command == Command::Reserve || request.command == Command::Configure)
        {
            if (!request.limitBytes || !request.ttlSeconds)
                return reply;
            mLimit = request.limitBytes;
            mTtl = std::chrono::seconds(request.ttlSeconds);
        }
        expire(now);
        trim(mLimit);
        const auto it = mEntries.find(request.key);

        switch (request.command)
        {
        case Command::Lookup:
            if (it != mEntries.end() && !it->second.writer)
            {
                it->second.lastUse = now;
                reply.result = Result::Success;
                reply.id = it->second.id;
                reply.bytes = it->second.bytes;
            }
            break;
        case Command::Reserve:
            reply = reserve(request, clientPid, it != mEntries.end(), now);
            break;
        case Command::Publish:
            if (it != mEntries.end() && it->second.writer && it->second.writerPid == clientPid && it->second.id == request.id)
            {
                it->second.lastUse = now;
                it->second.writer.reset();
                reply.result = Result::Success;
            }
            break;
        case Command::Erase:
            if (it != mEntries.end() && (request.id == 0 || request.id == it->second.id))
                mEntries.erase(it);
            reply.result = Result::Success;
            break;
        case Command::Configure:
            reply.result = Result::Success;
            break;
        case Command::Clear:
            mEntries.clear();
            reply.result = Result::Success;
            break;
        }
        return reply;
    }

private:
    void trim(uint64_t maxBytes)
    {
        uint64_t bytes = 0;
        for (const auto& [key, entry] : mEntries)
            bytes += entry.bytes;

        while (bytes > maxBytes)
        {
            const auto it = std::min_element(
                mEntries.begin(), mEntries.end(), [](const auto& a, const auto& b) { return a.second.lastUse < b.second.lastUse; }
            );
            bytes -= it->second.bytes;
            mEntries.erase(it);
        }
    }

    Reply reserve(const Request& request, uint32_t clientPid, bool exists, Clock::time_point now)
    {
        Reply reply;
        reply.pid = mPid;
        reply.result = Result::Rejected;
        if (exists || !request.bytes || request.bytes > mLimit)
            return reply;

        auto writer = ownHandle(OpenProcess(SYNCHRONIZE, FALSE, clientPid));
        if (!writer)
            return reply;

        trim(mLimit - request.bytes);
        const uint64_t id = ++mLastId;
        const auto name = mappingName(mPid, id);
        auto mapping = ownHandle(CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            &mSecurity,
            PAGE_READWRITE,
            static_cast<DWORD>(request.bytes >> 32),
            static_cast<DWORD>(request.bytes),
            name.c_str()
        ));
        if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS)
            return reply;

        mEntries.emplace(request.key, Entry{std::move(mapping), std::move(writer), clientPid, id, request.bytes, now});
        reply.result = Result::Success;
        reply.id = id;
        reply.bytes = request.bytes;
        return reply;
    }

    SECURITY_ATTRIBUTES& mSecurity;
    const uint32_t mPid = GetCurrentProcessId();
    uint64_t mLastId = static_cast<uint64_t>(Clock::now().time_since_epoch().count());
    uint64_t mLimit = 0;
    std::chrono::seconds mTtl{0};
    std::map<Key, Entry> mEntries;
};

bool waitForClient(HANDLE pipe, HANDLE event, Cache& cache, Clock::time_point lastRequest)
{
    ResetEvent(event);
    OVERLAPPED operation{};
    operation.hEvent = event;
    DWORD bytes = 0;
    if (ConnectNamedPipe(pipe, &operation) || GetLastError() == ERROR_PIPE_CONNECTED)
        return true;
    if (GetLastError() != ERROR_IO_PENDING)
        return false;

    while (true)
    {
        const auto wait = WaitForSingleObject(event, 250);
        if (wait == WAIT_OBJECT_0)
            return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;

        const auto now = Clock::now();
        cache.expire(now);
        if (wait != WAIT_TIMEOUT || (cache.empty() && now - lastRequest >= 5s))
        {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &bytes, TRUE);
            return false;
        }
    }
}

int run(const fs::path& stateDirectory)
{
    auto owner = ownHandle(CreateFileW(
        (stateDirectory / L"host.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr
    ));
    if (!owner)
        return 1;

    auto security = createUserSecurity();
    if (!security)
        return 1;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.get(), FALSE};
    const uint32_t pid = GetCurrentProcessId();
    auto pipe = ownHandle(CreateNamedPipeW(
        pipeName(pid).c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        1024,
        1024,
        0,
        &attributes
    ));
    auto event = ownHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!pipe || !event)
        return 1;

    const auto pidFile = stateDirectory / kPidFile;
    {
        std::ofstream file(pidFile, std::ios::trunc);
        file << pid << '\n';
        file.close();
        if (!file)
            return 1;
    }

    Cache cache(attributes);
    auto lastRequest = Clock::now();
    while (waitForClient(pipe.get(), event.get(), cache, lastRequest))
    {
        Request request;
        ULONG clientPid = 0;
        if (transfer(pipe.get(), event.get(), request, false) && request.protocol == kProtocol &&
            GetNamedPipeClientProcessId(pipe.get(), &clientPid))
        {
            lastRequest = Clock::now();
            auto reply = cache.apply(request, clientPid, lastRequest);
            if (transfer(pipe.get(), event.get(), reply, true))
            {
                // Let the client consume the reply before disconnecting the pipe.
                uint8_t acknowledgement = 0;
                transfer(pipe.get(), event.get(), acknowledgement, false);
            }
        }
        DisconnectNamedPipe(pipe.get());
    }

    std::error_code error;
    fs::remove(pidFile, error);
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    try
    {
        return argc == 2 ? run(argv[1]) : 1;
    }
    catch (...)
    {
        return 1;
    }
}
