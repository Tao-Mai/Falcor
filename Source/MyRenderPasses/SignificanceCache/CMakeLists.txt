add_plugin(SignificanceCache)

target_link_libraries(SignificanceCache PRIVATE MyRenderPassesShared)

target_sources(SignificanceCache PRIVATE
    SignificanceCache.cpp
    SignificanceCache.h
    SignificanceCache.cs.slang
    CacheTypes.slang
    CacheGuide.slang
    CosineLobe.slang
    UpdateCache.cs.slang
    SurfaceTracing.slang
    Tests/Lobes.cs.slang
)

target_copy_shaders(SignificanceCache MyRenderPasses/SignificanceCache)
target_source_group(SignificanceCache "MyRenderPasses")
