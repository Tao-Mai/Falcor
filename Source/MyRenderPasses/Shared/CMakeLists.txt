add_library(MyRenderPassesShared INTERFACE)

target_link_libraries(MyRenderPassesShared INTERFACE Falcor)
target_include_directories(MyRenderPassesShared INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/../.."
)

set(shared_shader_sources
    EmissiveLightHelpers.slang
)

add_custom_target(MyRenderPassesSharedShaders SOURCES ${shared_shader_sources})
target_copy_shaders(MyRenderPassesSharedShaders MyRenderPasses/Shared)
target_source_group(MyRenderPassesSharedShaders "MyRenderPasses")

if(FALCOR_COPY_SHADERS)
    set(shared_shader_outputs ${shared_shader_sources})
    list(TRANSFORM shared_shader_outputs PREPEND "${FALCOR_SHADER_OUTPUT_DIRECTORY}/MyRenderPasses/Shared/")
    target_sources(MyRenderPassesSharedShaders PRIVATE ${shared_shader_outputs})
endif()

add_dependencies(MyRenderPassesShared MyRenderPassesSharedShaders)
