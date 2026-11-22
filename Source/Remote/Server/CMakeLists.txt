add_falcor_executable(FalcorRemoteServer)
target_sources(FalcorRemoteServer PRIVATE
    RemoteRenderer.cpp RemoteRenderer.h RemoteCommands.cpp
    RemoteStream.cpp RemoteStream.h
)
target_link_libraries(FalcorRemoteServer PRIVATE args falcor_remote glfw CUDA::cuda_driver)
