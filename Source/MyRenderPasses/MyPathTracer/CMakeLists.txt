add_plugin(MyPathTracer)

target_sources(MyPathTracer PRIVATE
    MyPathTracer.cpp
    MyPathTracer.h
    MyPathTracer.rt.slang
)

target_copy_shaders(MyPathTracer MyRenderPasses/MyPathTracer)

target_source_group(MyPathTracer "MyRenderPasses")
