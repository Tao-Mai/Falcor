# PathReuse

## 参考论文

Philippe Bekaert、Mateu Sbert 和 John Halton 的 *Accelerating Path Tracing by Re-Using Paths* 发表于 2002 年第 13 届 Eurographics Workshop on Rendering，第 125–134 页。 [论文 DOI](https://doi.org/10.2312/EGWR/EGWR02/125-134)

论文把邻近像素的路径后缀接到当前像素的首个表面命中点。记接收像素的主命中为 $x_i$，源路径的第二个顶点为 $y_j$，重连后得到 $x_i \to y_j \to z_j \to \cdots$。对给定后缀，式 (12) 的 balance heuristic 按各采样策略的联合密度在组内归一化。该密度由采到 $y_j$ 的面积 PDF 和在 $y_j$ 生成后续方向的立体角 PDF 相乘得到，并计入 $x_i \to y_j$ 的可见性。第 3.4 节将复用扩展到显式采光，第 3.7 节用 N-rooks 排列和逐帧平移来减轻块状噪声。 [论文 PDF](https://diglib.eg.org/server/api/core/bitstreams/2707e04f-b9b6-4cbf-b6d6-7ae609c5e985/content)

## 实现

1. **逐像素追踪。** 从 `vbuffer` 读取主命中，每像素生成一条基础路径，保存 BSDF 后缀和从第二顶点可见的发光网格 NEE 样本。主表面的直接采光累加到 `localRadiance`。 [TracePaths.rt.slang](TracePaths.rt.slang) · [PathReuseSample.slang](PathReuseSample.slang)
2. **组内重连。** 每组最多 16 个像素，由 $8\times8$ 个计算线程遍历接收像素与源路径的组合。新连接 $x_i \to y_j$ 用 inline ray query 测试可见性，并将结果写入组共享内存，供 BSDF 后缀和 NEE 样本共用。跨像素重连用于普通三角网格上的连续反射，其余路径贡献按像素累加。 [ReusePaths.cs.slang](ReusePaths.cs.slang)
3. **Falcor 调度。** `PathReuse` 在同一个 RenderPass 内依次派发两个 shader，通过结构化缓冲区传递样本。输入为 `vbuffer` 和可选的 `viewW`，`color` 输出尺寸跟随 `vbuffer`。 [PathReuse.cpp](PathReuse.cpp) · [PathReuseParams.slangh](PathReuseParams.slangh)

论文第 3.4 节在复用显式采光路径时，以灯光采样的 PDF 作为第二步密度。本实现重连 $x_i \to y_j \to \ell_j$ 时，保留原路径在 $y_j$ 处采到的灯点 $\ell_j$。在重连后的 $y_j$，用 BSDF 和 NEE 的立体角 PDF 对发光项做本地 power heuristic MIS。组内 balance MIS 再分别归约两条后缀的联合密度。BSDF 后缀使用面积 PDF 乘 BSDF 方向 PDF，NEE 后缀使用面积 PDF 乘灯光方向 PDF。

`grouping` 可在脚本或渲染面板中选择。对比图默认使用 `N-rooks` 和 6 次间接反弹。 [对比图脚本](../../../scripts/my_graph/path_reuse.py)

| 分组 | 像素安排 |
|---|---|
| `Tiles` | 每个 $4\times4$ 连续像素块为一组。 |
| `N-rooks` | 每个 $16\times16$ 块划分为 16 组，每组在每行、每列各取一个像素。块网格随帧平移。 |

## 实验结果

在 RTX 2070 上用 Vulkan 无窗口渲染 CornellBox，分辨率为 512×288，固定像素中心，允许 6 次间接反弹。基线 `MyPathTracer` 使用发光网格 NEE 和 MIS。参考图使用 Falcor `PathTracer` 的 NEE、PowerTwo MIS 和 `AccumulatePass`，以每帧 16 spp 累积 1,024 帧，共 16,384 spp。由前后两半的差异估算，参考图的残余噪声 MSE 约为 `1.30e-6`。 [参考图脚本](../../../scripts/my_graph/path_reuse_target.py)

各档位在线性 RGB 上按像素和通道计算 MSE，再对 16 组测量取平均。GPU 时间为 `VBufferRT` 和路径追踪 pass 合计的每帧中位数。 [测量脚本](../../../scripts/my_graph/benchmark_path_reuse.py)

这组结果来自早期的组内联合 MIS 版本。

| 指标 | `MyPathTracer` | `PathReuse N-rooks` | `PathReuse Tiles` |
|---|---:|---:|---:|
| 1 spp MSE | 0.021584 | 0.004527 | 0.004176 |
| 3 spp MSE | 0.007107 | 0.001516 | 0.001395 |
| 4 spp MSE | 0.005337 | 0.001138 | 0.001045 |
| 16 spp MSE | 0.001323 | 0.000282 | 0.000261 |
| 64 spp MSE | 0.000333 | 0.0000714 | 0.0000661 |
| GPU 时间 | 2.29 ms | 6.18 ms | 5.91 ms |

- 同为 1 spp，`N-rooks` 的 MSE 低 79%。按每帧 GPU 中位数估算，基线 3 spp 用时约 6.88 ms，接近 `N-rooks` 1 spp 的 6.18 ms，此时复用的 MSE 仍低 36%。
- 1 spp 的间接光 MSE 从 `0.01848` 降至 `0.00143`。两者的直接光 MSE 都约为 `0.00310`，相当于 `N-rooks` 总 MSE 的 68%。主表面到灯的噪声因此仍然可见。
- `Tiles` 的数值误差略低，1 spp 时相邻像素的间接光亮度误差相关系数约为 `0.71`。`N-rooks` 的该系数接近 `0`，所以它更少出现成块的噪声。

### 本地 MIS 对照

沿用同一参考图和采样设置，各测 16 组。改成本地 MIS 后，每个重连对的共享记录从三个贡献项减为两个。 [对照数据](../../../build/path-reuse-mse/local-mis-comparison.json)

| 分组 | GPU 时间，联合 MIS → 本地 MIS | 1 spp MSE，联合 MIS → 本地 MIS |
|---|---:|---:|
| `N-rooks` | 6.49 → 5.30 ms | 0.004527 → 0.004493 |
| `Tiles` | 6.02 → 5.15 ms | 0.004176 → 0.004145 |

## 复现

在 Falcor 根目录运行。下面的命令使用 Release 版 Mogwai 和当前 Python 环境中的 NumPy。

```powershell
$env:FALCOR_DEVMODE = '1'
$env:PYTHONPATH = python -c 'from pathlib import Path; import numpy; print(Path(numpy.__file__).resolve().parent.parent)'
.\build\clion-release\bin\Release\Mogwai.exe --headless --device-type vulkan --width 512 --height 288 --script .\scripts\my_graph\benchmark_path_reuse.py
```

结果写入 `build/path-reuse-mse`，其中 `results.json` 保存逐组误差和计时，`linear_rgb.npz` 保存参考图和各方法的线性 RGB 均值图。 [逐组数据](../../../build/path-reuse-mse/results.json) · [线性 RGB 图像](../../../build/path-reuse-mse/linear_rgb.npz)
