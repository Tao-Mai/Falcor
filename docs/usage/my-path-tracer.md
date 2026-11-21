### [索引](../index.md) | [使用](./index.md) | MyPathTracer

--------

# MyPathTracer 与原生 RTXDI 对照实验

本项目以 Falcor 9.0（`b2104f39e4f53c9c19dad371332590e9adc41381`）为基础。
`MyPathTracer` 从 `MinimalPathTracer` 复制，保留相同的文件组织和初始算法。
原版 Render Pass、RTXDI 与 Falcor 框架算法源码不作修改。

当前研究 baseline 是 **Falcor 原生 `PathTracer` 开启 `useRTXDI=true`**，
`maxSurfaceBounces`、`maxDiffuseBounces`、`maxSpecularBounces`、`maxTransmissionBounces` 均为 0，
`samplesPerPixel=1`、`colorFormat=RGBA32F`、`useReSTIRPT=false`。
这是[官方 PathTracer 文档](./path-tracer.md)支持的 RTXDI 直接光照接入，RTXDI options 沿用原生默认值，包含时空重采样。
不声明严格复现 ReSTIR 2020 的算法参数、场景、相机或论文数值。
当前 MyPathTracer 与 baseline 的差距会直接保留，方便后续逐步实现自己的 ReSTIR。

## 场景有多少个

需要区分“场景文件”“资产集合”和“已经渲染验证的实验配置”：

| 范围 | 数量与含义 |
| --- | --- |
| Falcor 自带媒体 | 当前安装的媒体中可枚举 31 个 `.pyscene`：25 个功能测试、5 个逆向渲染、1 个 Arcade；不能都算作论文 benchmark |
| DI 论文相关公开资产 | Bistro、Emerald Square、Zero Day，生成 4 个 DI 配置（Bistro 分室内和室外） |
| DI 清单中的可用配置 | 6 个：4 个 DI 论文相关配置，加 Cornell、Arcade 两个验证场景 |
| 明确缺失的 ReSTIR 2020 场景 | 4 个：Burger Restaurant、Subway、Soda Hall、Amusement Park；报告中显示缺失原因，不用其他场景冒充 |

具体配置位于 `scripts/python/my_path_tracer/scene_catalog.json`，按以下套件选择：

| 套件 | 内容 |
| --- | --- |
| `smoke` | Cornell、Arcade，2 个快速验证场景 |
| `restir2020` | Bistro Exterior、Emerald Square、Zero Day，加 4 个明确缺失项；当前仅覆盖 7 类主场景中的 3 类公开资产 |
| `paper`（默认） | Bistro Exterior、Bistro Interior、Emerald Square、Zero Day，共 4 个 DI 配置；同时报告 4 个缺失项 |
| `all` | 6 个 DI / 验证配置和 4 个缺失项 |

Veach Ajar、Veach Bidir 和 Sun Temple 原先依据 ReSTIR PG 的全局光照实验加入，
不符合当前 DI 实验目的，现已从清单以及全部默认套件移除。
Falcor 9.0 的[官方 RTXDI 图像测试](https://github.com/NVIDIAGameWorks/Falcor/blob/9.0/tests/image_tests/renderpasses/test_RTXDI.py)
加载的是 Arcade，不是 Veach Ajar。
Bistro Interior 等 DI 论文场景的依据为 [Area ReSTIR 对比场景列表](https://graphics.cs.utah.edu/research/projects/area-restir/)。

“可用”指入口资产已安装，是否成功渲染由每次运行的 `suite.json` 记录。
公开资产采用重建实验配置：使用导入相机、固定时刻、关闭动画和景深。
首次场景更新可能用 FBX 相机的绑定姿态覆盖导入位姿，因此脚本在初始化后恢复导入的世界位姿，
并检查预热中的相机是否保持不变。FBX 配置的裁剪范围统一为 0.01 至 10000 米，原始范围也会记录。
每个场景都记录来源、许可证、论文位置、资产版本或归档哈希及配置差异。
这些配置采用公开资产的重建实验设置，具体版本和相机与论文可能不同。

## Windows 一键运行

在仓库根目录运行，不需要手动激活 Conda：

```bat
tools\run_my_path_tracer_benchmark.bat
```

入口使用本机 Conda `gs_env`，通过 Falcor 原生的 `FALCOR_USE_SYSTEM_PYTHON`
配置让 Python 绑定与该环境匹配，再使用生成的 `setpath.ps1` 设置运行路径。
默认构建目录是本机现有的 `build/clion-release`；`-BuildOnly` 只配置并编译，不运行实验。
日常命令和输出位置集中在根目录的 [命令.md](../../命令.md)。
首次配置仍遵循 Falcor 官方 `setup.bat` 和 CMake 流程。

常用命令：

```bat
rem 查看 ReSTIR 2020 的覆盖范围与缺失原因，不加载 GPU
 tools\run_my_path_tracer_benchmark.bat -Suite restir2020 -List

rem 运行全部 6 个 DI / 验证配置，并记录 4 个缺失项
 tools\run_my_path_tracer_benchmark.bat -Suite all

rem 快速检查两个小场景
 tools\run_my_path_tracer_benchmark.bat -Suite smoke -Profile smoke

rem 更高分辨率和采样帧数
 tools\run_my_path_tracer_benchmark.bat -Suite paper -Profile paper

rem 已构建时跳过构建；也可以选择自己的 CMake 构建目录
 tools\run_my_path_tracer_benchmark.bat -Suite paper -SkipBuild -BuildDir build/windows-ninja-msvc

rem 复查旧的独立 RTXDIPass 示例；Zero Day 可复现异常，不能混入新基线的统计
 tools\run_my_path_tracer_benchmark.bat -Scenes zero_day -BaselineRenderer rtxdi-pass -SkipBuild
```

默认 `preview` 使用 640×360、预热 32 帧、累积 64 帧；`paper` 使用
1280×720、预热 64 帧、累积 256 帧；`smoke` 使用 160×90、预热 4 帧、累积 4 帧。
`paper` 只是较高预算的配置名，不代表已经满足论文统计显著性或等时性能实验要求。
可用 `-Width`、`-Height`、`-Frames`、`-Warmup` 覆盖预算。

资产默认放在仓库同级 `FalcorResearchAssets`，可以通过 `-AssetsRoot` 或
`FALCOR_BENCHMARK_ASSETS` 环境变量指定位置。资产不提交到代码仓库，保留各来源许可证。
`-Output` 指定一个尚不存在的结果目录；省略时自动创建带 UTC 时间戳的目录。
使用 `-RequireComplete` 时，缺失场景也会导致非零退出码。

每个场景由独立子进程执行，结束后释放 GPU 场景资源；失败或超时会记录并继续其他场景。
运行中查看终端进度，以及套件的 `suite.json`、各场景的 `status.json` 和 `render.log`。

## Python 入口

使用本机 Conda `gs_env` 时，也可以遵循[官方 Python 文档](../falcor-in-python.md)，
在二进制目录执行生成的 `setpath.ps1`，然后运行：

```powershell
python -m pip install -r scripts/python/my_path_tracer/requirements.txt
python scripts/python/my_path_tracer/benchmark_suite.py --suite paper
python scripts/python/my_path_tracer/benchmark_suite.py --suite all --scenes bistro_exterior zero_day
```

通过 `--rtxdi-options options.json` 传入原生 RTXDI options 字典。
省略时使用原生默认值；实际生效参数会写入各场景的 `provenance.json`。
基线入口默认 `--baseline-renderer path-tracer`；`--baseline-renderer rtxdi-pass` 保留独立示例作诊断参照。
两种入口的完整材质着色与路径处理不同，结果和 GPU 时间不能混合比较，CSV 与图片明确标注入口。
套件默认只通过已有属性设置 MyPathTracer 的 `maxBounces=0`、`computeDirect=true`，
以直接光照作为初始对比目标。`--my-max-bounces` 可以调整原有路径深度，不改变算法实现。
原生 RTXDI 以反射直接光照为目标；透射、delta 材质、发光纹理近似等差异需结合场景解读，
当前报告不以“两种渲染器必须输出一致”为通过条件。

## 会输出哪些图和数据

结果位于忽略目录 `test-results/my_path_tracer/suite_<UTC>/`：

| 文件 | 含义 |
| --- | --- |
| `data_summary.png`、`.svg` | 可直接查看的 GPU 时间、MSE 距离、MAE 和状态数据总图 |
| `comparison_summary.png`、`.svg` | 每个 DI 场景的 RTXDI、MyPathTracer、差异图与数值汇总在一张图片中 |
| `index.html`（可选） | 仅显式传入 `--html` 或启动器 `-Html` 时生成 |
| `overview.png` | RTXDI / MyPathTracer 对照总图；异常配置使用 DIAGNOSTIC 标记 |
| `summary.csv`、`suite.json` | 汇总指标、参数、覆盖范围和运行状态 |
| `<场景>/comparison.png`、`.svg` | RTXDI、MyPathTracer、线性 HDR 差异热图并排展示 |
| `<场景>/baseline.png`、`my.png`、`difference.png` | 可分别查看或排版的图像 |
| `<场景>/baseline.exr`、`my.exr` | 原生 Falcor 导出的线性 HDR 累积结果 |
| `<场景>/squared_difference.exr` | 原生 ErrorMeasurePass 输出的平方差图 |
| `<场景>/curves.png`、`.svg` | 随累积帧数变化的 baseline 距离与 GPU pass 时间 |
| `<场景>/metrics.csv`、`.json` | 每帧对 baseline 的 RGB MSE、MAE 和最大绝对差 |
| `<场景>/profiler.json` | 原生 Profiler 的完整事件、逐帧 GPU/CPU 计时和统计量 |
| `<场景>/provenance.json` | 源码哈希、场景入口哈希、相机、光照、设备、有效参数与运行环境 |
| `<场景>/display.json` | 两张图共用的曝光和显示映射、差异图色标范围 |
| `<场景>/config.json`、`scene_import.pyscene`、`render.log` | 可审阅的实验配置、原生导入脚本和详细日志 |

若出现 NaN/Inf，保存原始异常帧和 `invalid_radiance.json`，另输出
`diagnostic_comparison.png`，用紫色标出无效像素。这是失败帧诊断图，
不计算有效 MSE/MAE，不将该场景改为成功；套件继续其他场景并返回非零退出码。
检查覆盖初始化、全部预热帧、全部测量帧和 profiler 收尾帧，同时读取两支原始输出与累积输出；
异常记录标明实际资源名、阶段和 renderer frame，避免把累积异常直接归因给某个算法。

两种渲染器共用确定性的 VBufferRT、相机、场景时刻和分辨率。
预热后只重置两个原生 AccumulatePass，保留 RTXDI 已预热的 reservoir 历史；
保存所请求累积区间的结果后，再额外执行一帧回收原生 GPU 时间戳。
初始化另外执行 1 帧，不计入采样与计时区间；配置明确记录初始化帧数和真正的 renderer frame 区间。
MyPathTracer 是每像素每帧一条路径；RTXDI 使用其候选采样和重用策略，不能把相同帧数称为相同光线预算。
当前原生 API 不暴露独立随机种子，这里不冒充多随机种子统计实验。

旧结果使用独立 `RTXDIPass` 示例；Zero Day 出现非有限值，失败记录仍保留。
核对官方文档与原始渲染图后，当前默认入口改为原生 `PathTracer + RTXDI` 的直接光照配置，
新旧结果分目录保存，并重新运行测量，不通过重绘将旧失败标记为成功。
详细依据、对照方法及上游 issue 的适用边界见 [Zero Day 接入核对记录](./my-path-tracer-zero-day.md)。
两种算法的图像差异本身不会触发失败。

默认产物为上述 PNG 总图，不需要打开网页。已有结果可直接重绘，无需重跑 GPU：

```powershell
python scripts/python/my_path_tracer/benchmark_suite.py --suite paper --replot test-results/my_path_tracer/rtxdi_comparison_verified --output test-results/my_path_tracer/di_summary_new
```

重绘只使用源运行实际记录的帧数、参数、数据和状态，在新目录记录源结果路径，不会将历史失败改为成功。

显示图对每个场景使用一组由 baseline 计算的公共曝光，再统一采用 Reinhard 和 sRGB 映射，
不会分别给两种算法自动调亮。误差数值始终使用显示变换前的线性 HDR 数据。

**RTXDI 是 baseline，不是真值。** 当前 MSE/MAE 是与 baseline 的距离，不能解释为精度、偏差或无偏性。
原生 Profiler 分开记录 MyPathTracer、RTXDIBaseline 和 VBufferRT 的 GPU scope；它们仍在一个共享图中运行，
会受到执行顺序和缓存影响，不能用这里的数字直接宣称论文级等时加速比。
高 SPP 真值、独立性能实验、多次随机运行和收敛验证应在对应研究算法稳定后另行定义。

## 源码结构与调试

| 位置 | 职责 |
| --- | --- |
| `Source/RenderPasses/MyPathTracer/MyPathTracer.h/.cpp` | 插件接口、属性、反射、执行、UI、场景绑定与 program vars，C++ 实现集中在一个 `.cpp` |
| `Source/RenderPasses/MyPathTracer/MyPathTracer.rt.slang` | 完整 shader：数据结构、着色、光源采样、路径追踪和全部 ray tracing 入口 |
| `scripts/MyPathTracer.py` | 可在 Mogwai 中运行的原生渲染图 |
| `scripts/python/my_path_tracer/` | 场景清单、原生渲染图、headless 实验和图像报告 |
| `tests/python_tests/renderpasses/` | GPU 等价性、场景清单和公共曝光回归测试 |
| `tests/image_tests/renderpasses/` | 原生 Mogwai 图像测试和测试渲染图 |
| `tools/run_my_path_tracer_benchmark.bat/.ps1` | Windows 构建、Python 环境和一键实验入口 |

插件遵循当前版本的 `add_plugin`、`registerPlugin`、`ref<Device>` 和 `Properties` API。
部分旧官方教程仍写 `add_renderpass`、`SharedPtr`，发生冲突时以当前 checkout 的模板为准。
shader 实现集中在 `.rt.slang`，保持原来的声明顺序和程序全局绑定。
构建注册和 shader 拷贝均使用 Falcor 原生 CMake 机制。

```powershell
cmake --build build/clion-release --config Release --target Mogwai MyPathTracer ImageCompare
$env:FALCOR_DEVMODE = '1'
build/clion-release/bin/Release/Mogwai.exe --device-type vulkan --script scripts/MyPathTracer.py --scene resource/CornellBox/CornellBox.pyscene
```

`FALCOR_DEVMODE=1` 从源码目录加载 shader，可使用 F5 重载。
C++ 断点使用对应 Debug 或 RelWithDebInfo 构建；shader 调试信息使用 Mogwai 的 `--debug-shaders`。
本机 Mogwai D3D12 在载入图之前就因预览版 Agility SDK / 设备创建失败而退出；Vulkan 已通过运行验证。
Python Testbed 的 D3D12 使用可用系统 runtime，已通过实际渲染。
详见[官方 Agility SDK 限制](../falcor-in-python.md#agility-sdk-limitations)，
以及 [Microsoft 对实验特性和 Developer Mode 的说明](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12enableexperimentalfeatures)。
这里不修改全局 Windows 设置或框架算法来绕过环境问题。

## 原始复制实现的等价性检查

`benchmark.py` 保留为 **MyPathTracer 与未修改 MinimalPathTracer 的行为回归**，
与新的 RTXDI 研究对比用途分开：

```powershell
python scripts/python/my_path_tracer/benchmark.py --frames 32 --warmup 8
python -m unittest discover -s tests/python_tests -p test_my_path_tracer.py -v
python -m unittest discover -s tests/python_tests -p test_benchmark_suite.py -v
python -m unittest discover -s tests/python_tests -p test_rtxdi_baseline.py -v
```

`test_rtxdi_baseline.py` 使用已安装的公开 Zero Day 资产进行 640×360、预热 32 帧、测量 64 帧的 GPU 回归，
检查所有阶段的四个输出及实际生效的 RTXDI / 零反弹参数；未安装外部资产时明确跳过。

也可使用 Falcor 原有 Python 测试入口配合官方 build preset：

```powershell
python tests/testing/run_python_tests.py --config windows-ninja-msvc-Release -p test_my_path_tracer.py -v
python tests/testing/run_image_tests.py --config windows-ninja-msvc-Release --filter renderpasses/test_MyPathTracer --run-only --parallel 1
```

已有验证包括：Release 编译；4 项 GPU 回归（32 帧配对输出）；Cornell/D3D12 的
256×144、32 帧原始 RGBA 完全一致，RGB MSE 和导出 EXR MSE 为零；Mogwai/Vulkan 原生图像测试。
Arcade/Vulkan 的共享输入对照有微小浮点差异，8 帧最大绝对差约 `3.8e-6`，
通过显式容差检查，不宣称所有 backend 都逐位一致。
对照图公共曝光、完整报告、异常诊断、DI 场景筛选和历史输入兼容工具另有 7 项测试。

## 资料与场景来源

- Falcor 9.0：[Render Pass 教程](https://github.com/NVIDIAGameWorks/Falcor/blob/9.0/docs/tutorials/02-implementing-a-render-pass.md)、[编码规范](https://github.com/NVIDIAGameWorks/Falcor/blob/9.0/docs/development/coding-conventions.md)、[CMake](https://github.com/NVIDIAGameWorks/Falcor/blob/9.0/docs/development/cmake.md)、[Python 接口](https://github.com/NVIDIAGameWorks/Falcor/blob/9.0/docs/falcor-in-python.md)。
- [Gradient ReSTIR 开源实现](https://github.com/elite-sheep/gradient-restir)：参考固定时钟、headless Testbed、预热和批量输出流程；不照搬其旧版 Falcor API。
- [ReSTIR 2020 项目页](https://benedikt-bitterli.me/restir/)、[Area ReSTIR 项目页](https://graphics.cs.utah.edu/research/projects/area-restir/)：DI 实验场景依据，具体对应关系见清单。
- [ORCA Bistro](https://developer.nvidia.com/orca/amazon-lumberyard-bistro)、[Emerald Square](https://developer.nvidia.com/orca/nvidia-emerald-square)、[Zero Day](https://developer.nvidia.com/orca/beeple-zero-day)：资产来源，原许可证随资产保留。
