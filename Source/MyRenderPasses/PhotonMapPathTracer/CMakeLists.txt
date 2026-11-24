add_plugin(PhotonMapPathTracer)

target_link_libraries(PhotonMapPathTracer PRIVATE MyRenderPassesShared)

target_sources(PhotonMapPathTracer PRIVATE
    PhotonMapPathTracer.cpp
    PhotonMapPathTracer.h
    PhotonMapPathTracer.cs.slang
    TracePhotons.cs.slang
    PhotonGuide.slang
    PhotonMap.slang
    SurfaceTracing.slang
)

target_copy_shaders(PhotonMapPathTracer MyRenderPasses/PhotonMapPathTracer)

target_source_group(PhotonMapPathTracer "MyRenderPasses")
