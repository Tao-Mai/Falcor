# 自定义渲染通道公共接口

跨通道复用的 Slang 模块和 C++ 头文件放在本目录。各通道在自己的 `CMakeLists.txt` 中添加公共目标依赖。

```cmake
target_link_libraries(YourPass PRIVATE MyRenderPassesShared)
```

Slang 通过完整模块路径导入。

```slang
import MyRenderPasses.Shared.EmissiveLightHelpers;
```

`EmissiveLightHelpers.slang` 提供发光网格采样点的 alpha 裁剪检查，两种接口均在采样点被裁掉时返回 `true`。调用前应已获得有效的发光三角形或光源样本。

- `isMaskedEmitter(triangleIndex, materialID, barycentrics)` 用于已知三角形、材质和三维重心坐标的采样点，例如光子发射点。
- `isMaskedLightSample(light)` 接收 Falcor 的 `TriangleLightSample`，供直接光采样使用。`light.uv` 表示重心坐标的后两维。

发射采样沿用官方接收端采样的接口层次，把固定接收点的采样改为光源起点和出射方向的采样。各入口接受已经选定的光源，选灯及其离散概率由调用方处理。

| 官方接收端接口 | 公共发射端接口 |
| --- | --- |
| `sampleLight(shadingPosW, light, sg, sample)` | `sampleLightEmission(light, sceneCenter, sceneRadius, sg, emission)` |
| `IEmissiveLightSampler.sampleLight(...)` | `sampleEmissiveLightEmission(triangleIndex, sg, emission)` |
| `EnvMapSampler.sample(...)` | `sampleEnvMapEmission(envSampler, sceneCenter, sceneRadius, sg, emission)` |

解析光入口直接接收官方 `LightData`，内部通过 `switch (LightType)` 分发到 `samplePointLightEmission()`、`sampleDirectionalLightEmission()`、`sampleDistantLightEmission()` 和矩形、圆盘、球形的 `sample*AreaLightEmission()`。这些具体函数接收二维随机数，与官方 `LightHelpers.slang` 的组织方式一致。网格入口接收已经选好的三角形，环境入口复用官方 `EnvMapSampler` 的方向重要性采样。

```slang
import Scene.Scene;
import Scene.Lights.LightData;
import MyRenderPasses.Shared.EmissiveLightHelpers;

// lightIndex 和 lightSelectionPdf 由当前渲染算法的选灯策略确定。
const LightData light = gScene.getLight(lightIndex);
LightEmissionSample emission;
if (sampleLightEmission(light, sceneCenter, sceneRadius, sg, emission))
{
    let ray = emission.ray;
    float3 throughput = emission.weight / lightSelectionPdf;
}
```

`sceneCenter` 和 `sceneRadius` 是世界坐标的场景包围球，仅用于无限远光源的发射圆盘。半径应略大于几何包围球，`PhotonMapPathTracer` 使用 AABB 包围球半径的 1.001 倍。

| 光源 | 发射位置和方向 | 未除路径数的通量权重 |
| --- | --- | --- |
| 发光三角形 | 灯面均匀采点，法线侧余弦半球 | $L_e\pi/p_A$ |
| 解析矩形、圆盘、球形灯 | 局部表面均匀采点，经变换后向世界空间法线侧采样余弦半球 | $L_e\pi/p_A$ |
| 点光、聚光 | 固定位置，球面或聚光锥内均匀采方向，求值官方同款半影衰减 | $I\,f/p_\omega$ |
| 方向光 | 场景包围球上游的投影圆盘，固定传播方向 | $I/p_A$ |
| 远距离光 `DistantLight` | 按光源张角采方向，再在对应的上游投影圆盘采点 | $I/p_A$ |
| 环境光 | Falcor `EnvMapSampler` 重要性采方向，向场景发射，在上游投影圆盘采点 | $L_e/(p_A p_\omega)$ |

表中权重只包含已选光源内的位置和方向采样。`DistantLight` 的 `intensity` 沿用 Falcor 的亮度归一化约定，改变张角不改变方向积分后的强度。零张角的远距离光作为方向 delta 处理，零张角聚光灯的总通量为零。解析面光使用面积变换的 Jacobian 计算 `positionPdf`，包含非均匀缩放后的椭球表面。

三个入口共用 `LightEmissionSample`。它只保存发射结果，光源身份由调用方传入的光源或三角形索引确定。发光纹理、alpha mask 和方向性光源配置在网格采样时求值。

| 字段 | 定义 |
| --- | --- |
| `posW` / `normalW` | 面光源为灯面点及法线，点光为灯的位置及朝向，无限远光源为投影圆盘点及传播方向 |
| `ray` | 发射射线，灯面起点已沿法线偏移 |
| `positionPdf` | 灯面或投影圆盘采点的面积密度，点光源用离散质量 1 |
| `directionPdf` | 出射方向的立体角密度，固定方向用离散质量 1 |
| `deltaPosition` / `deltaDirection` | 标记位置或方向的 delta 事件，供调用方区分连续密度与离散质量 |
| `weight` | 已除条件位置和方向 PDF 的 RGB 通量，各光源类型含义一致 |

调用方可用这些概率和 delta 标记计算路径的采样密度和 MIS 权重。无限远光源先采方向，再在该方向对应的圆盘上采位置，联合密度仍为两者的乘积。公共结果没有混用物理量的 `Le` 字段。

`PhotonMapPathTracer::emitPhoton()` 自行等概率选择有效光源类别，再在网格或解析类别内均匀选灯。它将 `weight` 除以类别和类内选灯的联合概率，再除以总发射路径数。其他通道可使用自己的选灯分布，公共模块不依赖具体算法的渲染开关或选灯策略。

CPU 端应先调用 `Scene::getLightCollection()` 准备发光三角形集合。环境光需要构造 `EnvMapSampler(device, scene->getEnvMap())` 并绑定到传入的 shader 变量，更换环境贴图时重建采样器。调用方只在对应光源存在且开启时调用其入口。

无可选光源时，调用方应停止采样。被遮罩裁掉的灯面点、零发光样本和全黑环境返回 `false`，该次抽样计零贡献，总抽样次数应包含失败样本。网格光按单面发射。球体、立方体等三角网格均由 `LightCollection` 覆盖，曲线、SDF 和自定义过程几何的发光表面没有接入此集合，需要各自的表面采样接口。

新增公共 Slang 模块时，将文件加入本目录 `CMakeLists.txt` 的 `shared_shader_sources`。启用 `FALCOR_COPY_SHADERS` 后，构建通道会将这些模块同步到运行目录的 `shaders/MyRenderPasses/Shared/`。开发模式使用源码目录中的模块。

C++ 公共头文件可通过 `#include "MyRenderPasses/Shared/文件名.h"` 引用，公共目标已提供对应的包含目录及 Falcor 依赖。
