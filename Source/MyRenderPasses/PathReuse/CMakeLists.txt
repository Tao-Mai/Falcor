add_plugin(PathReuse)

target_sources(PathReuse PRIVATE
    PathReuse.cpp
    PathReuse.h
    PathReuseParams.slangh
    PathReuseSample.slang
    TracePaths.rt.slang
    ReusePaths.cs.slang
)

target_copy_shaders(PathReuse MyRenderPasses/PathReuse)

target_source_group(PathReuse "MyRenderPasses")
