cmake_minimum_required(VERSION 3.15)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FalcorRemoteViewer LANGUAGES C CXX)
    set(GLFW_BUILD_EXAMPLES OFF)
    set(GLFW_BUILD_TESTS OFF)
    set(GLFW_BUILD_DOCS OFF)
    add_subdirectory(../../external/glfw glfw)
endif()

add_library(falcor_remote STATIC Server/RemoteTransport.cpp Server/RemoteTransport.h)
target_compile_features(falcor_remote PUBLIC cxx_std_17)
target_include_directories(falcor_remote PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Server)

set(GLFW_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/../../external/glfw/deps)
add_executable(FalcorRemoteViewer Client/RemoteViewer.cpp ${GLFW_DEPS}/glad_gl.c)
target_include_directories(FalcorRemoteViewer PRIVATE ${GLFW_DEPS})
target_link_libraries(FalcorRemoteViewer PRIVATE falcor_remote glfw)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "FalcorRemoteViewer supports Windows and Linux.")
endif()

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
# Headers for the NVIDIA driver APIs.
FetchContent_Declare(
    nv_codec_headers
    URL https://codeload.github.com/FFmpeg/nv-codec-headers/tar.gz/refs/tags/n13.0.19.0
    URL_HASH SHA256=86d15d1a7c0ac73a0eafdfc57bebfeba7da8264595bf531cf4d8db1c22940116
)
FetchContent_MakeAvailable(nv_codec_headers)
target_include_directories(falcor_remote SYSTEM PUBLIC ${nv_codec_headers_SOURCE_DIR}/include)
target_link_libraries(falcor_remote PUBLIC ${CMAKE_DL_LIBS})
if(WIN32)
    target_compile_definitions(falcor_remote PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(falcor_remote PUBLIC ws2_32)
    if(MINGW)
        target_link_options(FalcorRemoteViewer PRIVATE -static)
    endif()
endif()

if(FALCOR_RUNTIME_OUTPUT_DIRECTORY)
    set_target_properties(FalcorRemoteViewer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FALCOR_RUNTIME_OUTPUT_DIRECTORY})
endif()

if(TARGET Falcor)
    add_subdirectory(Server)
endif()
