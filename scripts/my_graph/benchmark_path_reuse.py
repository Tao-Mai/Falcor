"""用 Mogwai 无窗口渲染 CornellBox，在线性 RGB 上比较路径追踪误差。"""

import json
import sys
from pathlib import Path

import numpy as np
from falcor import RenderGraph


SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))
from path_reuse_target import render_graph_PathReuseTarget

SCENE = "resource/CornellBox/CornellBox.pyscene"
TARGET_SPP_PER_FRAME = 16
TARGET_FRAMES = 1024
MAX_BOUNCES = 6
TRIALS = 16
SAMPLE_COUNTS = (1, 3, 4, 16, 64)
WARMUP_FRAMES = 8
OUTPUT = "AccumulatePass.output"


def mse(image, target):
    delta = np.subtract(image, target, dtype=np.float64)
    return float(np.mean(np.square(delta)))


def neighbor_correlation(rgb_error):
    luminance = rgb_error @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)
    horizontal = np.corrcoef(luminance[:, :-1].ravel(), luminance[:, 1:].ravel())[0, 1]
    vertical = np.corrcoef(luminance[:-1, :].ravel(), luminance[1:, :].ravel())[0, 1]
    return float((horizontal + vertical) * 0.5)


def read_rgb(graph):
    image = graph.get_output(OUTPUT).to_numpy()[..., :3]
    if not np.isfinite(image).all():
        raise ValueError(f"{graph.name} 输出包含非有限值")
    return image


def render_frames(renderer, count):
    for _ in range(count):
        renderer.renderFrame()


def prepare(renderer, graph):
    renderer.addGraph(graph)
    renderer.setActiveGraph(graph)
    render_frames(renderer, WARMUP_FRAMES)
    return graph.get_pass("AccumulatePass")


def render_reference(renderer, bounces):
    graph = render_graph_PathReuseTarget(TARGET_SPP_PER_FRAME, bounces, "Center")
    graph.unmark_output("ToneMapper.dst")
    graph.mark_output(OUTPUT)
    accumulation = prepare(renderer, graph)
    accumulation.reset()
    render_frames(renderer, TARGET_FRAMES // 2)
    first_half = read_rgb(graph)
    print(f"参考图（{bounces} 次间接反弹）完成一半", flush=True)
    render_frames(renderer, TARGET_FRAMES // 2)
    target = read_rgb(graph)
    # 前半段与全样本均值的 MSE 等于两半均值 MSE 的四分之一。
    estimated_noise = mse(first_half, target)
    renderer.removeGraph(graph)
    print(f"参考图（{bounces} 次间接反弹）完成，残余噪声 MSE 约 {estimated_noise:.6g}", flush=True)
    return target, estimated_noise


def candidate_graph(name, pass_type, grouping, bounces):
    graph = RenderGraph(name)
    graph.create_pass("VBufferRT", "VBufferRT", {"samplePattern": "Center", "adjustShadingNormals": False})
    properties = {"maxBounces": bounces}
    if grouping is not None:
        properties["grouping"] = grouping
    graph.create_pass(pass_type, pass_type, properties)
    graph.create_pass("AccumulatePass", "AccumulatePass", {"enabled": True, "precisionMode": "SingleCompensated"})
    graph.add_edge("VBufferRT.vbuffer", f"{pass_type}.vbuffer")
    graph.add_edge("VBufferRT.viewW", f"{pass_type}.viewW")
    graph.add_edge(f"{pass_type}.color", "AccumulatePass.input")
    graph.mark_output(OUTPUT)
    return graph


def render_trial(renderer, graph, accumulation):
    accumulation.reset()
    previous_spp = 0
    images = {}
    for spp in SAMPLE_COUNTS:
        render_frames(renderer, spp - previous_spp)
        images[spp] = read_rgb(graph)
        previous_spp = spp
    return images


def direct_trials(renderer, pass_type, grouping):
    graph = candidate_graph(f"{pass_type}Direct", pass_type, grouping, 0)
    accumulation = prepare(renderer, graph)
    trials = [render_trial(renderer, graph, accumulation) for _ in range(TRIALS)]
    renderer.removeGraph(graph)
    return trials


def benchmark_method(renderer, name, pass_type, grouping, target, target_direct, direct_images):
    graph = candidate_graph(name, pass_type, grouping, MAX_BOUNCES)
    accumulation = prepare(renderer, graph)
    profiler = renderer.profiler
    profiler.start_capture(TRIALS * SAMPLE_COUNTS[-1])
    measurements = {spp: [] for spp in SAMPLE_COUNTS}
    image_sum = np.zeros_like(target, dtype=np.float64)
    indirect_correlations = []
    target_indirect = target - target_direct
    for direct_trial in direct_images:
        images = render_trial(renderer, graph, accumulation)
        for spp, image in images.items():
            direct = direct_trial[spp]
            indirect = image - direct
            measurements[spp].append(
                {
                    "rgb_mse": mse(image, target),
                    "direct_rgb_mse": mse(direct, target_direct),
                    "indirect_rgb_mse": mse(indirect, target_indirect),
                }
            )
            if spp == 1:
                indirect_correlations.append(neighbor_correlation(indirect - target_indirect))
        image_sum += images[SAMPLE_COUNTS[-1]]

    events = profiler.end_capture()["events"]
    profiler.enabled = False
    graph_event = "/onFrameRender/RenderGraphExe::execute()"
    trace_times = np.array(events[f"{graph_event}/{pass_type}/gpu_time"]["records"])
    primary_times = np.array(events[f"{graph_event}/VBufferRT/gpu_time"]["records"])
    # GPU 时间只统计主射线和路径追踪 pass，跳过采集开始处尚未回收的时间戳。
    gpu_times = (trace_times + primary_times)[2:]
    gpu_ms = float(np.median(gpu_times))
    renderer.removeGraph(graph)

    mean_image = image_sum / TRIALS
    result = {
        "median_gpu_ms_per_frame": gpu_ms,
        "indirect_luminance_error_neighbor_correlation_at_1_spp": float(np.mean(indirect_correlations)),
        "mse_by_spp": {
            spp: {
                key: float(np.mean([trial[key] for trial in trials]))
                for key in trials[0]
            }
            for spp, trials in measurements.items()
        },
        "trials": measurements,
        "mean_image_rgb_mse": mse(mean_image, target),
    }
    print(
        f"{name}: 1 spp MSE {result['mse_by_spp'][1]['rgb_mse']:.6g}，GPU {gpu_ms:.3f} ms/帧",
        flush=True,
    )
    return result, mean_image


def run_benchmark(renderer):
    root = SCRIPT_DIR.parents[1]
    output_dir = root / "build" / "path-reuse-mse"
    output_dir.mkdir(parents=True, exist_ok=True)
    renderer.loadScene(str(root / SCENE))
    target, target_noise = render_reference(renderer, MAX_BOUNCES)
    target_direct, direct_noise = render_reference(renderer, 0)

    # 0 次间接反弹只输出直接分量。每个 pass 从同一帧种子开始，便于逐样本相减。
    my_direct = direct_trials(renderer, "MyPathTracer", None)
    reuse_direct = direct_trials(renderer, "PathReuse", "N-rooks")
    methods = (
        ("MyPathTracer", "MyPathTracer", None, my_direct),
        ("PathReuseNRooks", "PathReuse", "N-rooks", reuse_direct),
        ("PathReuseTiles", "PathReuse", "Tiles", reuse_direct),
    )
    results = {}
    images = {"target": target, "target_direct": target_direct}
    for name, pass_type, grouping, direct in methods:
        results[name], images[name] = benchmark_method(
            renderer, name, pass_type, grouping, target, target_direct, direct
        )

    height, width, _ = target.shape
    report = {
        "scene": SCENE,
        "resolution": [width, height],
        "camera_sample_pattern": "Center",
        "max_bounces": MAX_BOUNCES,
        "target_spp": TARGET_FRAMES * TARGET_SPP_PER_FRAME,
        "target_rgb_noise_mse_estimate": target_noise,
        "direct_target_rgb_noise_mse_estimate": direct_noise,
        "trials": TRIALS,
        "warmup_frames": WARMUP_FRAMES,
        "mean_image_spp": TRIALS * SAMPLE_COUNTS[-1],
        "methods": results,
    }
    (output_dir / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    np.savez_compressed(output_dir / "linear_rgb.npz", **images)
    print(f"MSE 结果已保存到 {output_dir}", flush=True)


if "m" in globals():
    run_benchmark(m)
    exit()
