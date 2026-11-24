/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "ShaderModuleCache.h"
#include "Core/Platform/OS.h"
#include "Utils/Logger.h"

#include <nlohmann/json.hpp>
#include <slang-tag-version.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <future>
#include <map>
#include <thread>

namespace Falcor::ShaderModuleCache
{
namespace fs = std::filesystem;
using nlohmann::json;
constexpr uint32_t kMaxModuleManifestSize = 1024 * 1024;
constexpr uint64_t kMaxModuleCacheSize = 64 * 1024 * 1024;

bool supportsShaderModuleCache()
{
#ifdef SLANG_VERSION_NUMERIC
    // Older Slang releases can crash using a deserialized module during compilation.
    constexpr std::string_view version = SLANG_VERSION_NUMERIC;
    int year = 0;
    int release = 0;
    const auto end = version.data() + version.size();
    const auto parsedYear = std::from_chars(version.data(), end, year);
    if (parsedYear.ptr == end || *parsedYear.ptr != '.')
        return false;
    const auto parsedRelease = std::from_chars(parsedYear.ptr + 1, end, release);
    return parsedRelease.ec == std::errc{} && std::pair{year, release} >= std::pair{2026, 18};
#else
    return false;
#endif
}

static json getDependencyStamp(const fs::path& path)
{
    const auto utf8 = path.generic_u8string();
    return {std::string(utf8.begin(), utf8.end()), fs::last_write_time(path).time_since_epoch().count(), fs::file_size(path)};
}

bool restoreShaderModules(slang::ISession* session, const fs::path& path)
{
    if (path.empty())
        return true;

#ifdef SLANG_VERSION_NUMERIC
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        return true;

    try
    {
        const auto size = stream.tellg();
        if (size < std::streamoff(sizeof(uint32_t)) || size > std::streamoff(kMaxModuleCacheSize))
            return false;

        // Validate the complete archive before passing any bytes to Slang.
        std::vector<char> data(static_cast<size_t>(size));
        stream.seekg(0);
        if (!stream.read(data.data(), data.size()))
            return false;

        uint32_t manifestSize = 0;
        std::memcpy(&manifestSize, data.data(), sizeof(manifestSize));
        if (manifestSize > kMaxModuleManifestSize || manifestSize > data.size() - sizeof(manifestSize))
            return false;

        const auto* manifestStart = data.data() + sizeof(manifestSize);
        const auto manifest = json::parse(manifestStart, manifestStart + manifestSize);
        for (const auto& dependency : manifest.at("dependencies"))
        {
            if (getDependencyStamp(fs::u8path(dependency.at(0).get<std::string>())) != dependency)
                return false;
        }

        size_t offset = sizeof(manifestSize) + manifestSize;
        for (const auto& module : manifest.at("modules"))
        {
            const auto length = module.at("size").get<size_t>();
            if (!length || length > data.size() - offset)
                return false;
            offset += length;
        }
        if (offset != data.size())
            return false;

        offset = sizeof(manifestSize) + manifestSize;
        for (const auto& module : manifest.at("modules"))
        {
            const auto name = module.at("name").get_ref<const std::string&>().c_str();
            const auto source = module.at("source").get_ref<const std::string&>().c_str();
            const auto length = module.at("size").get<size_t>();
            Slang::ComPtr<slang::IBlob> blob;
            blob.attach(slang_createBlob(data.data() + offset, length));

            if (!blob || !session->loadModuleFromIRBlob(name, source, blob))
                return false;
            offset += length;
        }
        logDebug("Loaded {} Slang modules from {}", manifest.at("modules").size(), path.filename());
        return true;
    }
    catch (const std::exception& error)
    {
        logDebug("Could not read Slang module cache {}: {}", path, error.what());
        return false;
    }
#else
    return false;
#endif
}

bool storeShaderModules(slang::ICompileRequest* request, const fs::path& path)
{
    if (path.empty())
        return false;

    fs::path temporary;
    try
    {
        if (fs::exists(path))
            return false;

        Slang::ComPtr<slang::ISession> session;
        if (SLANG_FAILED(request->getSession(session.writeRef())) || !session->getLoadedModuleCount())
            return false;

        json dependencies = json::array();
        for (int i = 0; i < request->getDependencyFileCount(); ++i)
        {
            const fs::path dependency = request->getDependencyFilePath(i);
            if (fs::is_regular_file(dependency))
                dependencies.push_back(getDependencyStamp(dependency));
        }

        // Slang lists imported modules in dependency order.
        json modules = json::array();
        std::vector<Slang::ComPtr<slang::IBlob>> blobs;
        size_t totalSize = 0;
        for (SlangInt i = 0; i < session->getLoadedModuleCount(); ++i)
        {
            const auto module = session->getLoadedModule(i);
            const char* name = module->getName();
            const char* source = module->getFilePath();
            if (!name || !source || !fs::is_regular_file(source))
                return false;

            Slang::ComPtr<slang::IBlob> blob;
            if (SLANG_FAILED(module->serialize(blob.writeRef())) || !blob)
                return false;
            modules.push_back({{"name", name}, {"source", source}, {"size", blob->getBufferSize()}});
            totalSize += blob->getBufferSize();
            if (totalSize > kMaxModuleCacheSize)
                return false;
            blobs.push_back(std::move(blob));
        }

        const std::string manifest = json({{"dependencies", std::move(dependencies)}, {"modules", std::move(modules)}}).dump();
        if (manifest.size() > kMaxModuleManifestSize || totalSize + manifest.size() + sizeof(uint32_t) > kMaxModuleCacheSize)
            return false;

        fs::create_directories(path.parent_path());
        temporary = path;
        temporary += "." + getTempFilePath().filename().string() + ".tmp";
        std::ofstream stream;
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream.open(temporary, std::ios::binary | std::ios::trunc);
        const uint32_t manifestSize = static_cast<uint32_t>(manifest.size());
        stream.write(reinterpret_cast<const char*>(&manifestSize), sizeof(manifestSize));
        stream.write(manifest.data(), manifest.size());
        for (auto& blob : blobs)
            stream.write(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
        stream.close();
        fs::rename(temporary, path);
        return true;
    }
    catch (const std::exception& error)
    {
        std::error_code ignored;
        if (!temporary.empty())
            fs::remove(temporary, ignored);
        logDebug("Could not save Slang module cache {}: {}", path, error.what());
        return false;
    }
}

std::unordered_map<std::string, time_t> getShaderDependencyTimes(slang::ICompileRequest* request)
{
    std::unordered_map<std::string, time_t> times;
    const int count = request->getDependencyFileCount();
    for (int i = 0; i < count; ++i)
    {
        std::string path = request->getDependencyFilePath(i);
        if (fs::exists(path))
            times.emplace(path, getFileModifiedTime(path));
    }
    return times;
}

static fs::path getRecipePath(fs::path archive)
{
    archive += ".json";
    return archive;
}

void writeCompileRecipe(const fs::path& archive, const json& recipe)
{
    const auto path = getRecipePath(archive);
    fs::path temporary;
    try
    {
        if (fs::exists(path))
            return;
        fs::create_directories(path.parent_path());

        temporary = path;
        temporary += "." + getTempFilePath().filename().string() + ".tmp";
        std::ofstream stream;
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream.open(temporary, std::ios::binary | std::ios::trunc);
        stream << recipe;
        stream.close();
        fs::rename(temporary, path);
    }
    catch (const std::exception& error)
    {
        std::error_code ignored;
        if (!temporary.empty())
            fs::remove(temporary, ignored);
        logDebug("Could not save shader compile recipe {}: {}", path, error.what());
    }
}

void setCompileArguments(slang::ICompileRequest* request, const std::vector<std::string>& arguments)
{
    std::vector<const char*> argv;
    for (const auto& argument : arguments)
        argv.push_back(argument.c_str());
    if (!argv.empty())
        request->processCommandLineArguments(argv.data(), static_cast<int>(argv.size()));
}

static SlangRequest compileCachedProgram(slang::IGlobalSession* global, const fs::path& archive)
{
    try
    {
        std::ifstream stream(getRecipePath(archive), std::ios::binary);
        const auto recipe = json::parse(stream);
        if (recipe.at("compiler") != global->getBuildTagString())
            return {};

        const auto& targetOptions = recipe.at("target");
        slang::TargetDesc targetDesc;
        targetDesc.format = targetOptions.at("format").get<SlangCompileTarget>();
        targetDesc.profile = global->findProfile(targetOptions.at("profile").get_ref<const std::string&>().c_str());
        targetDesc.floatingPointMode = targetOptions.at("floatMode").get<SlangFloatingPointMode>();
        targetDesc.forceGLSLScalarBufferLayout = targetOptions.at("scalarLayout").get<bool>();

        const auto directories = recipe.at("searchPaths").get<std::vector<std::string>>();
        std::vector<const char*> searchPaths;
        for (const auto& directory : directories)
            searchPaths.push_back(directory.c_str());

        std::vector<slang::PreprocessorMacroDesc> defines;
        for (const auto& define : recipe.at("defines"))
            defines.push_back({define.at(0).get_ref<const std::string&>().c_str(), define.at(1).get_ref<const std::string&>().c_str()});

        std::vector<slang::CompilerOptionEntry> options;
        for (const auto& option : recipe.at("options"))
        {
            const auto name = option.at(0).get<slang::CompilerOptionName>();
            if (option.at(1).is_string())
            {
                const auto value = option.at(1).get_ref<const std::string&>().c_str();
                options.push_back({name, {slang::CompilerOptionValueKind::String, 0, 0, value, nullptr}});
            }
            else
            {
                options.push_back({name, {slang::CompilerOptionValueKind::Int, option.at(1).get<int>(), 0, nullptr, nullptr}});
            }
        }

        slang::SessionDesc sessionDesc;
        sessionDesc.targets = &targetDesc;
        sessionDesc.targetCount = 1;
        sessionDesc.searchPaths = searchPaths.data();
        sessionDesc.searchPathCount = static_cast<SlangInt>(searchPaths.size());
        sessionDesc.preprocessorMacros = defines.data();
        sessionDesc.preprocessorMacroCount = static_cast<SlangInt>(defines.size());
        sessionDesc.defaultMatrixLayoutMode = recipe.at("matrixLayout").get<SlangMatrixLayoutMode>();
        sessionDesc.compilerOptionEntries = options.data();
        sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(options.size());

        Slang::ComPtr<slang::ISession> session;
        if (SLANG_FAILED(global->createSession(sessionDesc, session.writeRef())) || !restoreShaderModules(session, archive))
            return {};

        SlangRequest request;
        if (SLANG_FAILED(session->createCompileRequest(request.writeRef())))
            return {};
        request->setCompileFlags(SLANG_COMPILE_FLAG_NO_CODEGEN);
        setCompileArguments(request, recipe.at("arguments").get<std::vector<std::string>>());

        for (const auto& module : recipe.at("sources"))
        {
            const auto& name = module.at(0).get_ref<const std::string&>();
            const int index = request->addTranslationUnit(SLANG_SOURCE_LANGUAGE_SLANG, name.empty() ? nullptr : name.c_str());
            for (const auto& source : module.at(1))
                request->addTranslationUnitSourceFile(index, source.get_ref<const std::string&>().c_str());
        }
        for (const auto& entry : recipe.at("entryPoints"))
        {
            request->addEntryPoint(
                entry.at(0).get<int>(), entry.at(1).get_ref<const std::string&>().c_str(), entry.at(2).get<SlangStage>()
            );
        }
        if (SLANG_FAILED(request->compile()))
            return {};
        return request;
    }
    catch (const std::exception& error)
    {
        logDebug("Could not preload shader compile recipe {}: {}", archive, error.what());
        return {};
    }
}

struct ShaderStartupPreload
{
    // Keep these sessions alive until the renderer releases the programs made from them.
    fs::path directory;
    Slang::ComPtr<slang::IGlobalSession> replayGlobal;
    std::vector<Slang::ComPtr<slang::IGlobalSession>> globals;
    std::map<fs::path, std::future<PreloadedShader>> requests;
    std::vector<std::future<void>> workers;
};

static std::unique_ptr<ShaderStartupPreload> gpShaderStartupPreload;

bool isShaderPreloadScheduled(const fs::path& archive)
{
    return gpShaderStartupPreload && gpShaderStartupPreload->requests.find(archive) != gpShaderStartupPreload->requests.end();
}

std::optional<PreloadedShader> takeShaderPreload(const fs::path& archive)
{
    if (!isShaderPreloadScheduled(archive))
        return {};
    auto job = gpShaderStartupPreload->requests.extract(archive);
    try
    {
        return job.mapped().get();
    }
    catch (const std::exception& error)
    {
        logDebug("Shader preload failed for {}: {}", archive, error.what());
        return PreloadedShader{};
    }
}

void startShaderCachePreload(const fs::path& directory)
{
    if (!supportsShaderModuleCache() || directory.empty())
        return;

    try
    {
        auto context = std::make_unique<ShaderStartupPreload>();
        context->directory = directory;
        gpShaderStartupPreload = std::move(context);
        if (!fs::is_directory(directory))
            return;

        std::vector<fs::directory_entry> archives;
        for (const auto& file : fs::directory_iterator(directory))
        {
            // Prime larger archives in the order they were first used.
            if (file.is_regular_file() && file.path().extension().empty() && file.file_size() >= 256 * 1024 &&
                fs::is_regular_file(getRecipePath(file.path())))
                archives.push_back(file);
        }
        if (archives.empty())
            return;

        std::sort(archives.begin(), archives.end(), [](const auto& a, const auto& b) { return a.last_write_time() < b.last_write_time(); });
        if (archives.size() > 16)
            archives.resize(16);

        const size_t workerCount = std::min({archives.size(), size_t{2}, std::max<size_t>(1, std::thread::hardware_concurrency() / 2)});
        auto* preload = gpShaderStartupPreload.get();
        preload->globals.resize(workerCount);
        struct Job
        {
            fs::path archive;
            std::promise<PreloadedShader> result;
        };
        std::vector<std::vector<Job>> groups(workerCount);
        for (size_t i = 0; i < archives.size(); ++i)
        {
            auto& job = groups[i % workerCount].emplace_back();
            job.archive = archives[i].path();
            preload->requests.emplace(job.archive, job.result.get_future());
        }
        for (size_t i = 0; i < groups.size(); ++i)
        {
            auto& global = preload->globals[i];
            preload->workers.emplace_back(std::async(std::launch::async, [&global, jobs = std::move(groups[i])]() mutable {
                std::vector<PreloadedShader> compiled(jobs.size());
                if (SLANG_SUCCEEDED(slang::createGlobalSession(global.writeRef())))
                {
                    for (size_t job = 0; job < jobs.size(); ++job)
                    {
                        auto request = compileCachedProgram(global, jobs[job].archive);
                        if (!request)
                            continue;

                        Slang::ComPtr<slang::IComponentType> scope;
                        if (SLANG_SUCCEEDED(request->getProgram(scope.writeRef())))
                            scope->getLayout();

                        compiled[job].dependencyTimes = getShaderDependencyTimes(request);
                        compiled[job].request = std::move(request);
                    }
                }

                // A Slang global session can be used by the render thread once this worker is done compiling.
                for (size_t job = 0; job < jobs.size(); ++job)
                    jobs[job].result.set_value(std::move(compiled[job]));
            }));
        }
    }
    catch (const std::exception& error)
    {
        gpShaderStartupPreload.reset();
        logDebug("Could not start shader cache preload {}: {}", directory, error.what());
    }
}

void stopShaderCachePreload()
{
    gpShaderStartupPreload.reset();
}

SlangRequest replayShaderModules(const fs::path& archive)
{
    if (!gpShaderStartupPreload || gpShaderStartupPreload->directory != archive.parent_path())
        return {};

    auto& global = gpShaderStartupPreload->replayGlobal;
    if (!global && SLANG_FAILED(slang::createGlobalSession(global.writeRef())))
        return {};
    return compileCachedProgram(global, archive);
}
} // namespace Falcor::ShaderModuleCache
