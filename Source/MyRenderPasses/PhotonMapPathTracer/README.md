# 基于光子图的重要性驱动路径追踪

`PhotonMapPathTracer` 先从光源追踪光子，再用附近光子的通量和局部 BRDF 构造相机路径的采样分布。这个两阶段方法来自 Henrik Wann Jensen 1995 年的论文《Importance Driven Path Tracing using the Photon Map》。[论文](https://doi.org/10.1007/978-3-7091-9430-0_31)

## 算法

1. **建图。** `emitPhoton()` 先等概率选有效光源类别，再在网格或解析类别内均匀选灯，然后分别调用公共 `sampleEmissiveLightEmission()`、`sampleLightEmission()` 或 `sampleEnvMapEmission()`。解析光入口接收官方 `LightData` 并按 `LightType` 分发，与官方接收端 `sampleLight()` 的结构对应。面光源采灯面点和余弦半球，点光/聚光采球面或光锥，方向光/远距离光/环境光在覆盖场景的投影圆盘上采起点。环境方向复用 Falcor 的 `EnvMapSampler` 重要性分布。公共结果 `weight` 仅包含条件位置/方向 PDF 和发射强度，每条光子路径的初始 RGB 通量为 `emission.weight / (selectionPdf * photonPaths)`，其中 `selectionPdf` 是本通道选择类别和光源的联合概率。只有网格光时退化为 $L_e\pi A N_L/N_P$。后续散射沿用 Falcor 的 BSDF 和俄罗斯轮盘赌。光子至少散射一次后，在带漫反射瓣的表面记录世界坐标、法线、来光方向和 RGB 通量。
2. **查询。** 用边长为两倍查询半径的 GPU 均匀网格索引光子，每格最多保留 32 条记录。着色时收集半径内、法线方向接近且与局部切平面距离足够近的光子，按 $1-d^2/r^2$ 衰减。将来光方向投到余弦半球的单位方形参数域，累积到 $4\times8$ 个方向区间。每个非空区间用中心方向的 BRDF 对 RGB 通量加权，再取亮度构造累计分布。
3. **估计。** 光子分支先按累计分布选区间，再在区间内均匀采样并映射回半球。相机路径用 $p=(1-\alpha)p_{\mathrm{BSDF}}+\alpha p_{\mathrm{photon}}$ 加权连续散射。离散 BSDF 事件按其分支概率 $1-\alpha$ 修正权重。发光网格的直接光采样和散射之后的发光面命中共享这个连续 PDF 做 MIS。光子图提供方向分布，像素辐亮度由实际追踪的相机路径估计。

设方向 $\omega$ 所在区间的概率质量为 $m_b$。每个区间占单位方形的 $1/32$，余弦参数化的立体角密度为 $\cos\theta/\pi$，因此 $p_{\mathrm{photon}}(\omega)=32\,m_b\cos\theta/\pi$。

相机路径和光子发射均支持发光网格、点光/聚光、解析矩形/圆盘/球形面光、方向光、远距离光与环境光，开关沿用场景的渲染设置。纯解析光或纯环境光场景也会建图。查询样本不足或分布为空时使用 BSDF 采样。相机移动时复用 GPU 上的光子图。更换场景、修改相关几何或材质、调整建图参数、增删或移动解析灯、改变灯光强度/属性、修改环境贴图及其强度/旋转后按需重建。更换环境贴图同时重建环境重要性采样器。高密度网格会触及每格容量，因而增加发射路径数对这些区域的收益有限。[公共发射接口](../Shared/README.md)

球体等三角网格发光体已由 `LightCollection` 覆盖。曲线、SDF 和自定义过程几何的发光表面尚无发射采样，当前也不支持体积散射。

## 使用与配置

构建 `Mogwai` 后按项目默认配置启动，在 `Active Graph` 下拉框选择 `PhotonMapPathTracer`。可在渲染通道面板调整采样参数，也可用 Python 的 `create_pass()` 或 `set_properties()` 设置。[渲染图](../../../scripts/PhotonMapPathTracer.py)

| 参数 | 默认值 | 含义与范围 |
| --- | ---: | --- |
| `enableGuiding` | `true` | 开启光子引导。关闭后可运行同一通道的 BSDF 对照 |
| `maxBounces` | 6 | 相机路径的最大间接反弹数，范围 0–16 |
| `guidedBounces` | 1 | 从主命中开始，最多有多少次反弹可使用引导，范围 0–16 |
| `photonPaths` | 131072 | 每次建图发射的路径数，范围 1–1048576 |
| `photonBounces` | 4 | 光子路径最多散射几次，范围 1–8 |
| `gatherRadius` | 0.04 | 查询半径占场景包围盒最长边的比例，范围 1/64–0.25 |
| `minPhotons` | 4 | 启用光子分支所需的最少有效邻近光子数，范围 1–32 |
| `guideMix` | 0.5 | 分布有效时选择光子分支的概率，范围 0–0.9 |
| `seed` / `photonSeed` | 0 / 0 | 相机路径与光子路径的独立种子，通过 Python 设置 |

## 实验设置

以下收敛和性能数据测于仅从网格光发射的版本，两个实验场景均只含网格光。新增光源类别的验证记录见后面的“光源覆盖验证”。

使用 RTX 2070（驱动 610.88），以 Vulkan Release 无窗口渲染，分辨率 256×144，固定像素中心，每帧 1 spp，最多 6 次间接反弹。比较关闭和开启引导的同一通道，引导组使用上表默认配置。每组 6 轮，各轮共享对应的相机种子，交替执行两组。引导组每轮换一个光子种子，并在首帧建图。

测试场景包括仓库的 CornellBox 和脚本生成的 UpwardEmitter。后者在天花板下放置朝上发光的小面光源和挡板，让墙面与地面主要接收天花板反射的光。参考图用关闭引导的通道，以另外两个种子各累计 1024 spp 后平均。误差为线性 RGB 的逐像素均方误差。

GPU 计时计入 `VBufferRT` 和 `PhotonMapPathTracer`，首帧包含光子预处理。各组先预编译着色器，再记录每轮的累计时间。表中的 MSE 取六轮均值，时间取六轮中位数。

## 实验结果

| 场景 | 采样 | 16 spp MSE | 64 spp MSE | 128 spp MSE | 20 ms MSE |
| --- | --- | ---: | ---: | ---: | ---: |
| CornellBox | BSDF | 1.343e−3 | 3.443e−4 | 1.747e−4 | 2.555e−4 |
| CornellBox | 光子引导 | 1.286e−3 | 3.261e−4 | 1.705e−4 | 1.467e−3 |
| UpwardEmitter | BSDF | 1.808e−2 | 4.638e−3 | 2.380e−3 | 3.657e−3 |
| UpwardEmitter | 光子引导 | 7.365e−3 | 1.941e−3 | 1.033e−3 | 1.075e−2 |

等时误差取两组共有的 20 ms GPU 预算，以累计 GPU 时间和 MSE 的对数曲线插值。UpwardEmitter 开启引导后，16、64、128 spp 的 MSE 分别降低 **59.3%、58.2%、56.6%**。CornellBox 的 128 spp MSE 降低 **2.4%**。两场景在 128 spp 时各组的平均亮度相对参考图的偏差绝对值均低于 0.011%。[UpwardEmitter 图像与收敛曲线](../../../build/photon-map-experiment/upward-emitter-comparison.png)

| 场景 | 采样 | 首帧 GPU 时间（ms） | 稳态 GPU 时间（ms/帧） |
| --- | --- | ---: | ---: |
| CornellBox | BSDF | 0.235 | 0.231 |
| CornellBox | 光子引导 | 1.502 | 1.406 |
| UpwardEmitter | BSDF | 0.245 | 0.244 |
| UpwardEmitter | 光子引导 | 1.931 | 1.827 |

稳态用时是每轮剔除首帧后的帧均值，再取轮间中位数。引导的稳态用时分别约为 BSDF 的 6.1 倍和 7.5 倍。逐着色点访问光子缓冲、分桶和求值 BRDF 增加了每帧开销，两场景在 20 ms 预算下均由 BSDF 对照取得更低误差。

Vulkan 无窗口测试通过了默认脚本注册、切换渲染图和图像读回。把 `maxBounces` 设为 0 后，开关引导的线性输出逐值一致。实验中每轮引导组的光子图均只在首帧构建。当前机器的 D3D12 验证停在 Falcor 创建设备阶段。[Vulkan 验证日志](../../../build/photon-map-experiment/startup-vulkan.out) · [D3D12 日志](../../../build/photon-map-experiment/startup-d3d12.out)

## 光源覆盖验证

发射接口按官方采样层次重构后，额外通过一次 16×16 的 Vulkan 短渲染检查，覆盖混合光源、灯光变化、环境贴图更换和光源开关。下面的功率和选灯比例数据保留自重构前的光源覆盖验证，本次没有重复运行功率实验。[接口重构验证日志](../../../build/all-light-emission/refactor.out)

新增光源类别使用 Vulkan Release 无窗口验证。以 64×64 分辨率分别检查纯点光、聚光、方向光、远距离光、环境光、混合光源和全黑环境。每个场景先渲染 8 帧，确认输出有限且只在首帧建图，再改变灯光或环境强度检查按需重建。验证脚本和结果保存在 `build/all-light-emission/`。

进一步检查了灯光移动、聚光角变化、解析灯激活/停用、环境旋转、替换为黑色/白色环境贴图，以及逐类关闭光源后重新只开启解析光。相关修改均触发一次重建，后续静态帧复用缓存，全部光源关闭时停止建图。[动态更新日志](../../../build/all-light-emission/updates.out) 原有不透明、alpha mask 和无光源场景的线性输出与扩展前相比，最大逐值差为 $2.98\times10^{-8}$。

另对每种发射方式生成 65536 个样本，检查方向单位长度、投影圆盘覆盖范围、有限通量和估计总功率。点光、矩形/圆盘/球形灯、方向光、远距离光和常量环境的总功率与解析值一致到浮点精度。带半影聚光灯的估计功率为 1.18831，数值积分为 1.19062，偏差约 0.19%。半轴为 2、2、4 的椭球灯估计功率为 269.825，解析值为 269.906，偏差约 0.03%。空场景、全黑环境和零张角聚光灯均返回零通量。网格、解析灯、环境同时启用时，三类样本数分别为 22003、21827、21706，符合各占三分之一的选类概率。[发射验证日志](../../../build/all-light-emission/sampler.out)

## 复现

在项目根目录运行以下命令。实验脚本使用 NumPy，Windows 下需让 Mogwai 的内嵌 Python 访问同版本 Python 的包目录。

```powershell
cmake --build build/clion-release --config Release --target Mogwai
$env:FALCOR_DEVMODE = '1'
$env:PYTHONPATH = (& python -c 'import site; print(site.getsitepackages()[-1])')
& ./build/clion-release/bin/Release/Mogwai.exe --headless --device-type vulkan --width 256 --height 144 --script scripts/my_graph/benchmark_photon_map.py | Out-Host
```

实验输出写入 `build/photon-map-experiment/`。JSON 记录逐轮误差、GPU 时间和建图帧，NPZ 保存参考图和第一轮的 16、128 spp 线性图像。[实验脚本](../../../scripts/my_graph/benchmark_photon_map.py) · [逐轮数据](../../../build/photon-map-experiment/results.json)
