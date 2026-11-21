### [使用](./index.md) | [对照实验](./my-path-tracer.md) | Zero Day 接入核对

# Zero Day 基线接入核对

核对日期：2026-11-17。Falcor 官方版本为 9.0（`759aad03`），本仓库 HEAD 为 `b2104f39`。
HEAD 相对官方版本的三个 C++ 改动涉及交换链与窗口呈现；RTXDI、RTXDIPass、PathTracer、材质和 GBuffer 算法源码没有差异。
运行目录内全部 Falcor Slang 文件也已与源码逐项比较，未发现不同内容。

## 官方文档与资产约定

- [Falcor 9.0 PathTracer 文档](https://github.com/NVIDIAGameWorks/Falcor/blob/9.0/docs/usage/path-tracer.md)：
  支持开启 RTXDI 采样直接光照；四类 bounce 上限为 0 时只计算直接光照；RTXDI 需要连接屏幕空间运动向量。
- [Falcor 官方 RTXDI 渲染图](https://github.com/NVIDIAGameWorks/Falcor/blob/9.0/scripts/RTXDI.py)：
  使用独立 RTXDIPass，连接 VBuffer 与运动向量，默认关闭累积，最后执行 ToneMapper。
- [Falcor 官方 RTXDI 图像测试](https://github.com/NVIDIAGameWorks/Falcor/blob/9.0/tests/image_tests/renderpasses/test_RTXDI.py)：
  使用 Arcade，检查第 1、16、64 帧；没有承诺任意 ORCA 资产的全部原始 HDR 像素均有限。
- [Falcor Python 环境文档](https://github.com/NVIDIAGameWorks/Falcor/blob/9.0/docs/falcor-in-python.md)：
  要求 Python ABI 匹配，并用生成的 setpath 配置路径；Agility SDK 另有宿主环境要求。
  本实验使用 Python 3.10 与实际构建对应的模块，不混用其他版本的 Python 扩展。
- [ORCA Zero Day](https://developer.nvidia.com/orca/beeple-zero-day)：
  发布的是 FBX / DDS 公共资产。下载包的 `CHANGELOG.txt` 明确标注针对 Falcor 4.0 优化，
  `README.txt` 规定金属粗糙度纹理通道与 DirectX 法线贴图。公共资产不能直接等同于论文的完整实验配置。
- 本版本实际依赖 `RTXDI 1.3.0-falcor`，集成约定以随包的 `external/packman/rtxdi/doc/Integration.md`
  和 `RtxdiApplicationBridge.md` 为准，不将最新 SDK 的接口套用到旧依赖。

## Issue 核对的结论边界

[Falcor issue #478](https://github.com/NVIDIAGameWorks/Falcor/issues/478) 讨论 BC5 法线的 RG 解码计算。
截至核对时，该项仍为 open，0 条评论，没有维护者确认或合并修复。
本地版本包含报告中提到的表达式，Zero Day 也使用 BC5 法线纹理，但这两点不能证明该 issue 是本次两个异常像素的原因。
本次没有修改该函数，没有将该问题表述为已经确认的 ReSTIR / RTXDI 重采样算法错误。

## 保留的对照证据

使用同一台 RTX 2070、D3D12、640×360、固定时刻 0，对照如下：

| 入口与设置 | 观察 |
| --- | --- |
| 直接加载官方 `scripts/RTXDI.py`，Arcade，65 帧 | 原始 RTXDIPass 输出全部有限 |
| 直接加载官方 `scripts/RTXDI.py`，Zero Day，保留默认场景动画状态，100 帧 | 原始输出复现 `(214,276)`、`(192,272)` 的非有限值；ToneMapper 的显示输出有限 |
| 独立 RTXDI 图，关闭动画并恢复导入相机 | 同类异常可复现，排除“双渲染器共享图”是唯一诱因 |
| 原生 PathTracer 开启 RTXDI，所有 bounce 上限 0，100 帧 | 原始 HDR 输出全部有限 |

这表明旧基线的“导入成功 + 显示正常”验证不足，不能据此推断全部原始 HDR 数据有效。
也不能将独立 RTXDIPass 与完整 PathTracer 的材质着色、贡献接纳条件和最终输出处理视为完全相同。
PathTracer 的直接光照代码只累加通过其原生贡献判断的值；实验脚本没有额外加入 NaN 替换或像素遮罩。

临时关闭法线校正只解决了第一个异常位置，延长到第 68 帧后仍失败，因此已撤回，未作为正式修复。
保留原始切线的实验也没有解决问题。没有删除材质、法线纹理、几何或失败像素来换取通过。

## 正式实验入口

默认基线采用官方文档支持的 `PathTracer`，设置 `useRTXDI=true`、`useReSTIRPT=false`、
所有 bounce 上限 0、每像素每帧 1 个样本、RGBA32F 路径颜色。
RTXDI options 不变；两支仍共享相同的 VBuffer、运动向量、相机和场景。
这是基线入口的修正，不宣称修复了独立 RTXDIPass 或 BC5 解码函数。

每次运行保存实际属性、源码哈希、四个输出的逐帧有限值检查结果以及原生 Profiler 数据。
新基线必须重新渲染，旧 RTXDIPass 结果不能与新结果拼接或覆盖。

```bat
tools\run_my_path_tracer_benchmark.bat -Suite paper -BaselineRenderer path-tracer
tools\run_my_path_tracer_benchmark.bat -Scenes zero_day -BaselineRenderer rtxdi-pass -SkipBuild
```

第二条用于复核旧入口，在当前机器和配置下会因严格的原始 HDR 检查返回失败，并保存异常帧。
所有比较指标均为与指定基线的距离，不是真值误差，也不将更换基线入口称为同一算法的性能改进。

## 本次验证结果

- 一键启动器运行 `paper` 套件：已安装的 4 个 DI 配置全部通过，0 失败，4 个缺失资产明确跳过。
  分辨率 640×360，预热 32 帧、测量 64 帧；每个场景检查 98 个 renderer frame 的四个输出。
  四个场景记录的源码哈希集合完全一致。
- Zero Day 额外运行 1280×720，预热 64 帧、测量 256 帧：322 个 renderer frame 的四个输出全部有限。
  与上述 640×360 运行使用同一组 shader 和脚本源码。
- 8 项 CPU 回归和 1 项使用真实 Zero Day 资产的 GPU 回归通过。
- 实测与图像分别保存在 `test-results/my_path_tracer/di_native_rtxdi_final/`
  和 `test-results/my_path_tracer/zero_day_native_rtxdi_720p/`；PNG / SVG 直接显示数据与图像对比，不依赖网页。
