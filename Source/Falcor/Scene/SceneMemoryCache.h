#pragma once

#include "SceneCache.h"

namespace Falcor
{
/**
 * Cache of serialized scenes and their image payloads in OS shared memory.
 * On Windows a Mogwai-managed host owns the mappings across application launches.
 */
class FALCOR_API SceneMemoryCache
{
public:
    struct Config
    {
        bool enabled = false;
        uint32_t limitMiB = 4096;
        uint32_t ttlSeconds = 30 * 60;
    };

    static bool isSupported();
    static bool configure(const Config& config);
    static bool clear();

    static ref<Scene> load(ref<Device> pDevice, const SceneCache::Key& key, const Config& config);
    static void store(const Scene::SceneData& sceneData, const SceneCache::Key& key, const Config& config);
    static void erase(const SceneCache::Key& key);

    /// The disk cache predates per-asset metadata. Check its timestamp before promoting it to memory.
    static bool canUseDiskCache(const Scene::SceneData& sceneData, const SceneCache::Key& key);
};
} // namespace Falcor
