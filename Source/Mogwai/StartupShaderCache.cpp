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
#include "StartupShaderCache.h"
#include "Core/Platform/OS.h"
#include "Core/Program/ShaderModuleCache.h"
#include "Utils/CryptoUtils.h"
#include "Utils/Logger.h"

#include <fmt/ranges.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

namespace Mogwai
{
namespace
{
namespace fs = std::filesystem;

bool isSourceFile(const fs::path& path)
{
    constexpr std::string_view extensions[] = {
        ".slang", ".slangh", ".hlsl", ".hlsli", ".glsl", ".cpp", ".cc", ".c", ".h", ".hpp", ".inl", ".py", ".pyscene", ".cmake",
    };
    const auto extension = path.extension().string();
    return path.filename() == "CMakeLists.txt" || std::find(std::begin(extensions), std::end(extensions), extension) != std::end(extensions);
}

void appendDirectory(std::vector<fs::directory_entry>& files, const fs::path& directory)
{
    if (!fs::is_directory(directory))
        return;

    for (const auto& entry : fs::recursive_directory_iterator(directory))
    {
        if (isSourceFile(entry.path()) && entry.is_regular_file())
            files.push_back(entry);
    }
}

std::string getSourceRevision()
{
    const auto& project = Falcor::getProjectDirectory();
    const auto& runtime = Falcor::getRuntimeDirectory();

    std::vector<fs::directory_entry> files;
    for (const auto* path : {"CMakeLists.txt", "dependencies.xml"})
    {
        if (fs::is_regular_file(project / path))
            files.emplace_back(project / path);
    }
    for (const auto* directory : {"Source", "cmake", "scripts", "resource"})
        appendDirectory(files, project / directory);
    appendDirectory(files, runtime / "shaders");
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path() < b.path(); });

    Falcor::SHA1 hash;
    hash.update("mogwai-startup-cache-1");
    hash.update(Falcor::isDevelopmentMode());
    for (const auto& file : files)
    {
        const auto path = file.path().generic_u8string();
        hash.update(path.size());
        hash.update(path.data(), path.size());
        hash.update(file.last_write_time().time_since_epoch().count());
        hash.update(file.file_size());
    }
    const auto digest = hash.finalize();
    return fmt::format("{:02x}", fmt::join(digest, ""));
}

bool isCacheGeneration(std::string_view name)
{
    constexpr std::string_view hex = "0123456789abcdef";
    // Accept old cache generation names that omitted leading zeroes.
    constexpr auto bytes = Falcor::SHA1::MD{}.size();
    return name.size() >= bytes && name.size() <= bytes * 2 && name.find_first_not_of(hex) == std::string_view::npos;
}
} // namespace

StartupShaderCache::StartupShaderCache(Falcor::Device::Desc& deviceDesc)
{
    if (deviceDesc.type != Falcor::Device::Type::Vulkan || deviceDesc.shaderCachePath.empty())
        return;

    try
    {
        const auto root = fs::absolute(fs::path(deviceDesc.shaderCachePath)) / "mogwai";
        const auto revision = getSourceRevision();
        const auto generation = root / revision;

        fs::create_directories(root);
        Falcor::LockFile directoryLock(root / "lock");
        if (!directoryLock.lock())
            return;

        if (!mGenerationLock.open(root / (revision + ".lock")) || !mGenerationLock.tryLock(Falcor::LockFile::LockType::Shared))
            return;

        for (const auto& entry : fs::directory_iterator(root))
        {
            const auto name = entry.path().filename().string();
            if (!isCacheGeneration(name) || name == revision || !fs::is_directory(entry.symlink_status()))
                continue;

            const auto lockPath = root / (name + ".lock");
            if (!fs::is_regular_file(lockPath))
                continue;

            Falcor::LockFile staleLock(lockPath);
            if (!staleLock.tryLock())
                continue;

            std::error_code error;
            fs::remove_all(entry.path(), error);
            staleLock.close();
            if (!error)
                fs::remove(lockPath, error);
        }

        deviceDesc.shaderModuleCachePath = generation / "modules";
        deviceDesc.shaderCachePath = (generation / "gfx").string();
        Falcor::ShaderModuleCache::startShaderCachePreload(deviceDesc.shaderModuleCachePath);
    }
    catch (const fs::filesystem_error& error)
    {
        Falcor::logWarning("Mogwai shader cache initialization failed: {}", error.what());
    }
}

StartupShaderCache::~StartupShaderCache()
{
    Falcor::ShaderModuleCache::stopShaderCachePreload();
}

} // namespace Mogwai
