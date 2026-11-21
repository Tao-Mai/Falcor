# Shader 编译记录

Mogwai 的 `--shader-compile-db <JSON 路径>` 开启真实编译输入导出。CLion 的 Mogwai 运行配置已将路径设为 `build/clion-release/shader_compile_commands.json`。未指定参数时不导出。

`ProgramManager::createSlangCompileRequest()` 在提交源文件和入口时记录实际搜索路径、全局及程序宏、目标、shader model、工具链版本、模块源码和类型一致性信息。同一程序的新配置覆盖旧配置，其他已运行程序的记录保留。文件通过临时文件替换，编辑器不会读到半个 JSON。

数据库是数组，每条记录包含：

- `directory`、`file`：编译工作目录和入口源码的解析后绝对路径。
- `arguments`：本次构建的 Slang 编译器路径和实际编译选项，包含运行时的 `-I`、`-D`、`-profile`、`-target`。各翻译单元及入口另存于下面的字段。
- `modules`：`{name, sources}` 列表；文件源使用 `{file}`，内存源使用 `{path, text}`，保留每个翻译单元的源顺序。
- `entryPoints`：入口名称、模块索引和 Slang stage。
- `compilerOptions`、`typeConformances`：Slang 会话附加选项及程序类型一致性信息。
- `compilerVersion`、`capturedAt`：实际链接的 Slang 版本和捕获时间。

这些是已执行编译的上下文，不能替代尚未运行的场景配置。宏来自实际的 Scene、MaterialSystem 和 SampleGenerator，不需要编辑器猜测。项目使用部署目录时，编辑器可结合构建系统的复制记录映射回源码。更新记录后重启编辑器语言服务以重新读取。
