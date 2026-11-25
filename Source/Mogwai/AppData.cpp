/***************************************************************************
 # Copyright (c) 2015-22, NVIDIA CORPORATION. All rights reserved.
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
#include "Falcor.h"
#include "AppData.h"
#include "Core/Platform/LockFile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <set>
#include <string_view>

using json = nlohmann::json;

using namespace Falcor;

namespace Mogwai
{
    namespace
    {
        const char kRecentScripts[] = "recentScripts";
        const char kRecentScenes[] = "recentScenes";

        constexpr size_t kMaxRecentFiles = 25;
        // Scene files scanned in the project resources. Standalone models can be loaded explicitly.
        constexpr std::array<std::string_view, 7> kSceneExtensions = {"pyscene", "pbrt", "xml", "usd", "usda", "usdc", "usdz"};
    }

    AppData::AppData(const std::filesystem::path& path)
        : mPath(path.lexically_normal())
    {
        // Make sure directories exist.
        std::filesystem::create_directories(mPath.parent_path());

        loadFromFile(mPath);
    }

    std::vector<std::filesystem::path> AppData::getRecentScenes() const
    {
        return {mRecentScenes.begin(), mRecentScenes.begin() + std::min(mRecentScenes.size(), kMaxRecentFiles)};
    }

    std::vector<std::filesystem::path> AppData::getAvailableScenes(const std::filesystem::path& resourceDirectory) const
    {
        namespace fs = std::filesystem;
        std::set<fs::path> scenes;
        const auto& filters = Scene::getFileExtensionFilters();
        const auto addScene = [&](const fs::path& path, const std::string& extension)
        {
            if (std::none_of(filters.begin(), filters.end(), [&](const auto& filter) { return filter.ext == extension; })) return;

            std::error_code error;
            auto canonicalPath = fs::canonical(path, error);
            if (!error && fs::is_regular_file(canonicalPath, error)) scenes.insert(std::move(canonicalPath));
        };

        for (const auto& path : mRecentScenes) addScene(path, getExtensionFromPath(path));

        std::error_code error;
        fs::recursive_directory_iterator entry(resourceDirectory, fs::directory_options::skip_permission_denied, error);
        for (const fs::recursive_directory_iterator end; !error && entry != end; entry.increment(error))
        {
            const auto extension = getExtensionFromPath(entry->path());
            if (std::find(kSceneExtensions.begin(), kSceneExtensions.end(), extension) != kSceneExtensions.end())
                addScene(entry->path(), extension);
        }

        return {scenes.begin(), scenes.end()};
    }

    void AppData::addRecentScript(const std::filesystem::path& path)
    {
        addRecentPath(mRecentScripts, path, kMaxRecentFiles);
    }

    void AppData::addRecentScene(const std::filesystem::path& path)
    {
        addRecentPath(mRecentScenes, path, mRecentScenes.max_size());
    }

    std::optional<SceneMemoryCache::Config> AppData::loadSceneMemoryCacheConfig() const
    {
        const auto path = mPath.parent_path() / "MogwaiSceneMemoryCache.json";
        std::ifstream file(path);
        if (!file)
            return {};

        try
        {
            const auto data = json::parse(file);
            SceneMemoryCache::Config config;
            config.enabled = data.value("enabled", config.enabled);
            config.limitMiB = data.value("limitMiB", config.limitMiB);
            config.ttlSeconds = data.value("ttlSeconds", config.ttlSeconds);
            FALCOR_CHECK(config.limitMiB > 0 && config.ttlSeconds > 0, "Cache limit and expiration must be positive.");
            return config;
        }
        catch (const std::exception& error)
        {
            logWarning("Failed to read settings from '{}': {}", path, error.what());
            return {};
        }
    }

    void AppData::saveSceneMemoryCacheConfig(const SceneMemoryCache::Config& config) const
    {
        const auto directory = mPath.parent_path();
        LockFile lock(directory / "MogwaiSceneMemoryCache.lock");
        FALCOR_CHECK(lock.lock(), "Could not lock the scene memory cache settings.");

        const auto temporary = directory / "MogwaiSceneMemoryCache.json.tmp";
        std::ofstream file(temporary);
        const json data = {{"enabled", config.enabled}, {"limitMiB", config.limitMiB}, {"ttlSeconds", config.ttlSeconds}};
        file << data.dump(4);
        file.close();
        FALCOR_CHECK(file.good(), "Could not save scene memory cache settings to '{}'.", temporary);
        std::filesystem::rename(temporary, directory / "MogwaiSceneMemoryCache.json");
    }

    void AppData::addRecentPath(std::vector<std::filesystem::path>& paths, const std::filesystem::path& path, size_t limit)
    {
        std::error_code error;
        const auto fullPath = std::filesystem::canonical(path, error);
        if (error || (!paths.empty() && paths.front() == fullPath)) return;

        paths.erase(std::remove(paths.begin(), paths.end(), fullPath), paths.end());
        paths.insert(paths.begin(), fullPath);
        if (paths.size() > limit) paths.resize(limit);
        save();
    }

    void AppData::removeNonExistingPaths(std::vector<std::filesystem::path>& paths)
    {
        paths.erase(std::remove_if(paths.begin(), paths.end(), [](const auto& path) {
            std::error_code error;
            auto canonicalPath = std::filesystem::canonical(path, error);
            return error || path != canonicalPath;
        }), paths.end());
    }

    void AppData::save()
    {
        saveToFile(mPath);
    }

    void AppData::loadFromFile(const std::filesystem::path& path)
    {
        std::ifstream ifs(path);
        if (!ifs.good()) return;

        auto readPathArray = [](const json& j)
        {
            std::vector<std::string> strings = j.get<std::vector<std::string>>();
            std::vector<std::filesystem::path> paths;
            for (const auto& str : strings) paths.push_back(std::filesystem::u8path(str));
            return paths;
        };

        try
        {
            const json j = json::parse(ifs);
            mRecentScripts = readPathArray(j[kRecentScripts]);
            mRecentScenes = readPathArray(j[kRecentScenes]);
        }
        catch (const std::exception& e)
        {
            logWarning("Failed to parse Mogwai settings file '{}': {}", path, e.what());
        }

        removeNonExistingPaths(mRecentScripts);
        removeNonExistingPaths(mRecentScenes);
    }

    void AppData::saveToFile(const std::filesystem::path& path)
    {
        auto getPathArray = [](const std::vector<std::filesystem::path>& paths)
        {
            std::vector<std::string> strings;
            for (const auto& path : paths)
            {
                const auto utf8 = path.u8string();
                strings.emplace_back(utf8.begin(), utf8.end());
            }
            return json(strings);
        };

        json j = json::object();
        j[kRecentScripts] = getPathArray(mRecentScripts);
        j[kRecentScenes] = getPathArray(mRecentScenes);

        std::ofstream ofs(path);
        if (!ofs.good()) return;

        ofs << j.dump(4);
    }
}
