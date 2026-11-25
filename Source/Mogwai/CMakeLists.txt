add_falcor_executable(Mogwai)

target_sources(Mogwai PRIVATE
    AppData.cpp
    AppData.h
    Mogwai.cpp
    Mogwai.h
    MogwaiScripting.cpp
    MogwaiSettings.cpp
    MogwaiSettings.h
    StartupShaderCache.cpp
    StartupShaderCache.h

    Extensions/Capture/CaptureTrigger.cpp
    Extensions/Capture/CaptureTrigger.h
    Extensions/Capture/FrameCapture.cpp
    Extensions/Capture/FrameCapture.h
    Extensions/Profiler/TimingCapture.cpp
    Extensions/Profiler/TimingCapture.h
)

target_link_libraries(Mogwai PRIVATE args)

if(WIN32)
    add_executable(MogwaiSceneCacheHost SceneMemoryCacheHost.cpp)
    target_compile_features(MogwaiSceneCacheHost PRIVATE cxx_std_17)
    target_include_directories(MogwaiSceneCacheHost PRIVATE ../Falcor)
    target_link_libraries(MogwaiSceneCacheHost PRIVATE advapi32)
    set_target_properties(MogwaiSceneCacheHost PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FALCOR_RUNTIME_OUTPUT_DIRECTORY})
    add_dependencies(Mogwai MogwaiSceneCacheHost)
endif()

target_source_group(Mogwai "/")
