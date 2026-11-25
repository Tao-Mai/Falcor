#include "SceneMemoryCache.h"
#include "Core/Platform/OS.h"
#include "Utils/Logger.h"

#if FALCOR_WINDOWS
#include "SceneMemoryCacheIPC.h"
#include "Core/Platform/LockFile.h"

#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <vector>
#endif

namespace Falcor
{
#if FALCOR_WINDOWS
namespace
{
namespace fs = std::filesystem;
using namespace SceneMemoryCacheIPC;
using json = nlohmann::json;

constexpr uint32_t kPackMagic = 0x53434D46;
constexpr uint32_t kPackVersion = 1;

struct PackHeader
{
    uint32_t magic = kPackMagic;
    uint32_t version = kPackVersion;
    uint64_t build = 0;
    uint64_t manifestBytes = 0;
    uint64_t sceneBytes = 0;
};

struct Dependency
{
    fs::path path;
    uint64_t size;
    fs::file_time_type modified;
    bool image;
};

class ReadOnlyStreamBuffer : public std::streambuf
{
public:
    ReadOnlyStreamBuffer(const uint8_t* data, size_t size)
    {
        auto* begin = reinterpret_cast<char*>(const_cast<uint8_t*>(data));
        setg(begin, begin, begin + size);
    }
};

const fs::path& stateDirectory()
{
    static const fs::path path = []()
    {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        return getAppDataDirectory() / "NVIDIA/Falcor/MogwaiSceneCache" / std::to_string(session);
    }();
    return path;
}

std::optional<Reply> callExisting(Request request)
{
    uint32_t pid = 0;
    std::ifstream pidFile(stateDirectory() / kPidFile);
    if (!(pidFile >> pid) || pid == 0)
        return {};
    pidFile.close();

    const auto name = pipeName(pid);
    auto openPipe = [&]()
    {
        return ownHandle(CreateFileW(
            name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr
        ));
    };
    auto pipe = openPipe();
    if (!pipe && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeW(name.c_str(), kIoTimeoutMs))
        pipe = openPipe();
    ULONG serverPid = 0;
    if (!pipe || !GetNamedPipeServerProcessId(pipe.get(), &serverPid) || serverPid != pid)
        return {};

    DWORD mode = PIPE_READMODE_MESSAGE;
    auto event = ownHandle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Reply reply;
    if (!event || !SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr) ||
        !transfer(pipe.get(), event.get(), request, true) || !transfer(pipe.get(), event.get(), reply, false) ||
        reply.protocol != kProtocol || reply.pid != pid)
        return {};

    uint8_t acknowledgement = 1;
    transfer(pipe.get(), event.get(), acknowledgement, true);
    return reply;
}

bool hasHostOwner()
{
    const auto file = ownHandle(CreateFileW(
        (stateDirectory() / L"host.lock").c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    ));
    return !file && GetLastError() == ERROR_SHARING_VIOLATION;
}

std::optional<Reply> callHost(const Request& request)
{
    if (auto reply = callExisting(request))
        return reply;

    fs::create_directories(stateDirectory());
    LockFile launchLock(stateDirectory() / "launch.lock");
    FALCOR_CHECK(launchLock.lock(), "Could not lock the scene cache host launch file.");
    if (auto reply = callExisting(request))
        return reply;

    const auto executable = getExecutableDirectory() / kHostExecutable;
    for (uint32_t attempt = 0; attempt < 2; ++attempt)
    {
        auto command = L"\"" + executable.wstring() + L"\" \"" + stateDirectory().wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        FALCOR_CHECK(
            CreateProcessW(
                executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process
            ),
            "Could not start '{}' (Windows error {}).",
            executable,
            GetLastError()
        );
        auto host = ownHandle(process.hProcess);
        auto thread = ownHandle(process.hThread);

        for (uint32_t wait = 0; wait < 100; ++wait)
        {
            if (auto reply = callExisting(request))
                return reply;
            if (WaitForSingleObject(host.get(), 20) == WAIT_OBJECT_0)
                break;
        }
    }
    return {};
}

Request makeRequest(Command command, const Key& key, const SceneMemoryCache::Config& config)
{
    Request request;
    request.command = command;
    request.key = key;
    request.limitBytes = uint64_t(config.limitMiB) * 1024 * 1024;
    request.ttlSeconds = config.ttlSeconds;
    return request;
}

bool eraseEntry(const Key& key, uint64_t id)
{
    Request request;
    request.command = Command::Erase;
    request.key = key;
    request.id = id;
    const auto reply = callExisting(request);
    return reply && reply->result == Result::Success;
}

View openMapping(const Reply& reply, DWORD access)
{
    const auto name = mappingName(reply.pid, reply.id);
    const auto mapping = ownHandle(OpenFileMappingW(access, FALSE, name.c_str()));
    return mapping ? View(MapViewOfFile(mapping.get(), access, 0, 0, size_t(reply.bytes))) : View{};
}

uint64_t buildSignature()
{
    static const uint64_t signature = []()
    {
        SHA1 hash;
        for (const auto& path : {getExecutablePath(), getExecutableDirectory() / "Falcor.dll"})
        {
            auto utf8 = path.u8string();
            hash.update(utf8.data(), utf8.size());
            std::error_code error;
            auto modified = fs::last_write_time(path, error);
            if (!error)
                hash.update(modified.time_since_epoch().count());
        }
        const auto digest = hash.finalize();
        uint64_t value = 0;
        std::memcpy(&value, digest.data(), sizeof(value));
        return value;
    }();
    return signature;
}

std::vector<Dependency> collectDependencies(const Scene::SceneData& sceneData)
{
    std::map<fs::path, bool> paths;
    for (const auto& path : sceneData.importPaths)
        paths.emplace(path, false);

    auto addImage = [&](const ref<Texture>& texture)
    {
        if (texture)
        {
            FALCOR_CHECK(!texture->getSourcePath().empty(), "Scene has a texture without a reusable source file.");
            paths[texture->getSourcePath()] = true;
        }
    };
    for (const auto& material : sceneData.pMaterials->getMaterials())
        for (uint32_t slot = 0; slot < uint32_t(Material::TextureSlot::Count); ++slot)
            addImage(material->getTexture(Material::TextureSlot(slot)));
    if (sceneData.pEnvMap)
        addImage(sceneData.pEnvMap->getEnvMap());

    std::vector<Dependency> files;
    files.reserve(paths.size());
    for (const auto& [path, image] : paths)
    {
        const auto size = fs::file_size(path);
        FALCOR_CHECK(!image || size > 0, "Image '{}' is empty.", path);
        files.push_back({path, size, fs::last_write_time(path), image});
    }
    return files;
}

bool matchesFile(const Dependency& dependency)
{
    std::error_code error;
    const auto modified = fs::last_write_time(dependency.path, error);
    return !error && modified == dependency.modified && fs::file_size(dependency.path, error) == dependency.size && !error;
}
} // namespace
#endif

bool SceneMemoryCache::isSupported()
{
    return FALCOR_WINDOWS != 0;
}

bool SceneMemoryCache::configure(const Config& config)
{
#if FALCOR_WINDOWS
    const auto reply = callExisting(makeRequest(Command::Configure, {}, config));
    return reply ? reply->result == Result::Success : !hasHostOwner();
#else
    return false;
#endif
}

bool SceneMemoryCache::clear()
{
#if FALCOR_WINDOWS
    Request request;
    request.command = Command::Clear;
    const auto reply = callExisting(request);
    if (reply ? reply->result != Result::Success : hasHostOwner())
        return false;
    logInfo("Cleared the scene memory cache.");
    return true;
#else
    return false;
#endif
}

void SceneMemoryCache::erase(const SceneCache::Key& key)
{
#if FALCOR_WINDOWS
    eraseEntry(key, 0);
#endif
}

bool SceneMemoryCache::canUseDiskCache(const Scene::SceneData& sceneData, const SceneCache::Key& key)
{
#if FALCOR_WINDOWS
    try
    {
        const auto diskCacheTime = std::filesystem::last_write_time(SceneCache::getCachePath(key));
        for (const auto& dependency : collectDependencies(sceneData))
            if (dependency.modified > diskCacheTime)
                return false;
    }
    catch (const std::exception&)
    {
        return false;
    }
#endif
    return true;
}

ref<Scene> SceneMemoryCache::load(ref<Device> pDevice, const SceneCache::Key& key, const Config& config)
{
#if FALCOR_WINDOWS
    const auto reply = callExisting(makeRequest(Command::Lookup, key, config));
    if (!reply || reply->result != Result::Success)
        return nullptr;
    auto view = openMapping(*reply, FILE_MAP_READ);
    if (!view)
        return nullptr;

    try
    {
        const auto* data = static_cast<const uint8_t*>(view.get());
        PackHeader header;
        FALCOR_CHECK(reply->bytes >= sizeof(header), "Truncated scene memory cache.");
        std::memcpy(&header, data, sizeof(header));
        FALCOR_CHECK(header.magic == kPackMagic && header.version == kPackVersion && header.build == buildSignature(), "Scene cache build changed.");
        const auto bodyBytes = reply->bytes - sizeof(header);
        FALCOR_CHECK(header.manifestBytes <= bodyBytes && header.sceneBytes <= bodyBytes - header.manifestBytes, "Invalid scene cache size.");
        const auto* manifestStart = data + sizeof(header);
        const auto* sceneStart = manifestStart + header.manifestBytes;
        auto manifest = json::parse(manifestStart, sceneStart);

        uint64_t offset = sizeof(header) + header.manifestBytes + header.sceneBytes;
        SceneCache::Images images;
        for (const auto& node : manifest)
        {
            Dependency file{
                fs::u8path(node.at("path").get<std::string>()),
                node.at("size").get<uint64_t>(),
                fs::file_time_type(fs::file_time_type::duration(node.at("modified").get<int64_t>())),
                node.at("image").get<bool>(),
            };
            FALCOR_CHECK(matchesFile(file), "Scene asset '{}' changed.", file.path);
            if (file.image)
            {
                FALCOR_CHECK(file.size > 0 && file.size <= reply->bytes - offset, "Invalid cached image size.");
                images.emplace(file.path, fstd::span<const uint8_t>(data + offset, size_t(file.size)));
                offset += file.size;
            }
        }
        FALCOR_CHECK(offset == reply->bytes, "Invalid scene cache asset table.");

        ReadOnlyStreamBuffer buffer(sceneStart, size_t(header.sceneBytes));
        std::istream stream(&buffer);
        auto scene = Scene::create(pDevice, SceneCache::readCacheStream(pDevice, stream, &images));
        logInfo("Scene memory cache hit ({} bytes).", reply->bytes);
        return scene;
    }
    catch (const std::exception& error)
    {
        eraseEntry(key, reply->id);
        logInfo("Invalidated a scene memory cache entry: {}", error.what());
    }
#endif
    return nullptr;
}

void SceneMemoryCache::store(const Scene::SceneData& sceneData, const SceneCache::Key& key, const Config& config)
{
#if FALCOR_WINDOWS
    Request request = makeRequest(Command::Reserve, key, config);
    uint64_t reservedId = 0;
    try
    {
        const auto dependencies = collectDependencies(sceneData);
        uint64_t textureBytes = 0;
        auto manifest = json::array();
        for (const auto& file : dependencies)
        {
            if (file.image)
            {
                if (file.size > request.limitBytes - textureBytes)
                    return;
                textureBytes += file.size;
            }
            manifest.push_back({
                {"path", file.path.u8string()},
                {"size", file.size},
                {"modified", file.modified.time_since_epoch().count()},
                {"image", file.image},
            });
        }
        const auto manifestText = manifest.dump();

        std::string sceneBytes;
        {
            std::ostringstream stream(std::ios::binary);
            SceneCache::writeCacheStream(stream, sceneData);
            sceneBytes = stream.str();
        }

        PackHeader header;
        header.build = buildSignature();
        header.manifestBytes = manifestText.size();
        header.sceneBytes = sceneBytes.size();
        request.bytes = sizeof(header) + header.manifestBytes + header.sceneBytes + textureBytes;
        if (request.bytes > request.limitBytes)
            return;

        const auto reply = callHost(request);
        FALCOR_CHECK(reply.has_value(), "Scene cache host did not respond.");
        if (reply->result != Result::Success)
            return;
        reservedId = reply->id;
        auto view = openMapping(*reply, FILE_MAP_WRITE);
        FALCOR_CHECK(view != nullptr, "Could not open the scene cache mapping.");

        auto* destination = static_cast<char*>(view.get());
        std::memcpy(destination, &header, sizeof(header));
        destination += sizeof(header);
        std::memcpy(destination, manifestText.data(), manifestText.size());
        destination += manifestText.size();
        std::memcpy(destination, sceneBytes.data(), sceneBytes.size());
        destination += sceneBytes.size();
        for (const auto& dependency : dependencies)
        {
            if (dependency.image)
            {
                std::ifstream file(dependency.path, std::ios::binary);
                file.read(destination, std::streamsize(dependency.size));
                FALCOR_CHECK(file.good(), "Could not cache image '{}'.", dependency.path);
                destination += dependency.size;
            }
            FALCOR_CHECK(matchesFile(dependency), "Scene asset '{}' changed while caching.", dependency.path);
        }
        view.reset();

        request.command = Command::Publish;
        request.id = reservedId;
        const auto published = callExisting(request);
        if (published && published->result == Result::Success)
            logInfo("Stored scene memory cache entry ({} bytes).", request.bytes);
        else
            eraseEntry(key, reservedId);
    }
    catch (const std::exception& error)
    {
        if (reservedId != 0)
            eraseEntry(key, reservedId);
        logWarning("Could not cache the scene in memory: {}", error.what());
    }
#endif
}
} // namespace Falcor
