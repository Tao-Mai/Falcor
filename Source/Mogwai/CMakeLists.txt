add_falcor_executable(Mogwai)

target_sources(Mogwai PRIVATE
    AppData.cpp
    AppData.h
    Mogwai.cpp
    Mogwai.h
    MogwaiScripting.cpp
    MogwaiSettings.cpp
    MogwaiSettings.h

    Extensions/Capture/CaptureTrigger.cpp
    Extensions/Capture/CaptureTrigger.h
    Extensions/Capture/FrameCapture.cpp
    Extensions/Capture/FrameCapture.h
    Extensions/Profiler/TimingCapture.cpp
    Extensions/Profiler/TimingCapture.h
)

target_link_libraries(Mogwai PRIVATE args)

if(FALCOR_ENABLE_REMOTE)
    target_sources(Mogwai PRIVATE RemoteStream.cpp RemoteStream.h)
    target_compile_definitions(Mogwai PRIVATE FALCOR_ENABLE_REMOTE=1)
    target_link_libraries(Mogwai PRIVATE falcor_remote glfw CUDA::cuda_driver)
endif()

target_source_group(Mogwai "/")
