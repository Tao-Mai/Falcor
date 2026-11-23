# 运行时 Shader 的代码辅助

CLion 的 Mogwai 运行配置会把运行时编译记录写入 `build/clion-release/shader_compile_commands.json`。从命令行启动时，可传入 `--shader-compile-db <活动 CMake 构建目录>/shader_compile_commands.json` 开启导出。

运行需要编辑的渲染图和场景，触发相应的 shader 编译。Research Workbench 从活动 CMake 构建目录读取记录，启动语言服务时优先选用与 Falcor 的 `slangc` 同目录的 `slangd`。记录更新后，已打开的语言服务会自动应用当前文件对应的搜索路径和宏。

记录来自 Falcor 实际提交给 Slang 的编译请求，包含目标、shader model、宏、搜索路径、已解析的源文件、具名内存模块、入口及 Slang 版本。启用 `FALCOR_DEVMODE=1` 时，项目内的 shader 优先从 `Source/` 读取，语言服务可直接跳转到源码。

每次运行首次捕获时生成新的记录文件。同一次运行中，相同模块和入口的编译更新对应记录，其他记录保留。文件完整写入后才替换，编辑器可以在 Mogwai 运行时读取。导出需要能关联到磁盘 shader 文件的编译请求。
