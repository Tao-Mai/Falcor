add_falcor_executable(FalcorRemoteServer)
target_sources(FalcorRemoteServer PRIVATE
    RemoteRenderer.cpp RemoteRenderer.h RemoteCommands.cpp
    RemoteStream.cpp RemoteStream.h
    PreviewEncoder.cpp PreviewEncoder.h PackPreview.cs.slang
)
target_link_libraries(FalcorRemoteServer PRIVATE args falcor_remote glfw CUDA::cuda_driver CUDA::nvjpeg)
target_copy_shaders(FalcorRemoteServer Remote)
