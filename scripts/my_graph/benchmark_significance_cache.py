"""Vulkan 无窗口对照实验：线性 RGB MSE、独立种子、冷缓存累计 GPU 时间。"""

import json
import os
import traceback
from pathlib import Path

import numpy as np
from falcor import RenderGraph, exit as exit_mogwai


ROOT = Path(__file__).resolve().parents[2]
OUTPUT_DIR = ROOT / "build" / "significance-cache-experiment"
OUTPUT = "AccumulatePass.output"
TRACER = "SignificanceCache"
CHECKPOINTS = (1, 4, 8, 16, 32, 64, 128, 256)
REFERENCE_SPP = 8192
TRIALS = 6
LUMINANCE = np.array([0.2126, 0.7152, 0.0722])
SETTINGS = {
    "maxBounces": 6,
    "gridResolution": 16,
    "warmupFrames": 4,
    "updateInterval": 4,
    "guideMix": 0.5,
    "maxExponent": 64.0,
    "minRoughness": 0.05,
    "queryLobes": int(os.environ.get("SC_QUERY_LOBES", "4")),
}
METHODS = {
    "BSDF": {"enableGuiding": False},
    "SC_primary": {"enableGuiding": True, "guidedBounces": 1},
    "SC_all": {"enableGuiding": True, "guidedBounces": 6},
}


def make_graph(name, props, tracer=TRACER):
    graph = RenderGraph(name)
    graph.create_pass("VBufferRT", "VBufferRT", {"samplePattern": "Center", "adjustShadingNormals": False})
    graph.create_pass("Tracer", tracer, props)
    graph.create_pass("AccumulatePass", "AccumulatePass", {"enabled": True, "precisionMode": "SingleCompensated"})
    graph.add_edge("VBufferRT.vbuffer", "Tracer.vbuffer")
    graph.add_edge("VBufferRT.viewW", "Tracer.viewW")
    graph.add_edge("Tracer.color", "AccumulatePass.input")
    graph.mark_output(OUTPUT)
    if tracer == TRACER:
        graph.mark_output("Tracer.guide")
    return graph


def render_frames(renderer, frames):
    for _ in range(frames):
        renderer.renderFrame()


def read_rgb(graph):
    image = graph.get_output(OUTPUT).to_numpy()[..., :3].copy()
    if not np.isfinite(image).all():
        raise ValueError(f"{graph.name} 输出包含非有限值")
    return image


def mse(image, reference):
    return float(np.mean(np.square(np.subtract(image, reference, dtype=np.float64))))


def mean_luminance(image):
    return float(np.mean(image, axis=(0, 1), dtype=np.float64) @ LUMINANCE)


def start_case(renderer, graph, **properties):
    renderer.setActiveGraph(graph)
    graph.get_pass("Tracer").set_properties(properties)
    graph.get_pass("AccumulatePass").reset()


def guide_stats(graph):
    guide = graph.get_output("Tracer.guide").to_numpy()
    if not np.isfinite(guide).all():
        raise ValueError("引导诊断输出包含非有限值")
    active = guide[..., 2] > 0
    return {
        "active_pixel_fraction": float(np.mean(active)),
        "eligible_pixel_fraction": float(np.mean(guide[..., 3] > 0)),
        "mean_lobes": float(np.mean(guide[..., 0][active])) if np.any(active) else 0.0,
        "mean_exponent": float(np.mean(guide[..., 1][active])) if np.any(active) else 0.0,
    }


def baseline_check(renderer):
    measurements = {}
    for bounces in (0, 6):
        images = []
        for tracer in ("MyPathTracer", "PhotonMapPathTracer", TRACER):
            props = {"maxBounces": bounces}
            if tracer == "PhotonMapPathTracer":
                props["enableGuiding"] = False
            elif tracer == TRACER:
                props["enableGuiding"] = bounces == 0
            graph = make_graph("Check_" + tracer, props, tracer)
            renderer.addGraph(graph)
            renderer.setActiveGraph(graph)
            render_frames(renderer, 16)
            images.append(read_rgb(graph))
            renderer.removeGraph(graph)
        difference = float(np.max(np.abs(images[1] - images[2])))
        if not np.allclose(images[1], images[2], rtol=1e-5, atol=1e-6):
            raise AssertionError(f"{bounces} 次反弹的 inline 基线差异过大：{difference}")
        relative_energy = mean_luminance(images[2]) / mean_luminance(images[0]) - 1
        if abs(relative_energy) > 0.01:
            raise AssertionError(f"与 MyPathTracer 的平均亮度差异过大：{relative_energy}")
        measurements[str(bounces)] = {
            "inline_baseline_mse": mse(images[1], images[2]),
            "inline_baseline_max_absolute_difference": difference,
            "my_path_tracer_mse": mse(images[0], images[2]),
            "my_path_tracer_relative_luminance_difference": relative_energy,
        }
    return measurements


def render_reference(renderer, graph, spp):
    halves = []
    for seed in (0x40000000, 0x50000000):
        start_case(renderer, graph, seed=seed)
        render_frames(renderer, spp // 2)
        halves.append(read_rgb(graph))
    return 0.5 * (halves[0] + halves[1]), mse(*halves) / 4


def capture_trial(renderer, graph, props, trial, checkpoints):
    start_case(renderer, graph, seed=(trial + 1) << 16)
    profiler = renderer.profiler
    scope = "/onFrameRender/RenderGraphExe::execute()"
    update_event = f"{scope}/Tracer/UpdateCache/gpu_time"
    reset_event = f"{scope}/Tracer/ResetCache/gpu_time"
    images, diagnostics, update_frames, reset_frames = [], {}, [], []
    profiler.start_capture(checkpoints[-1] + 1)
    for spp in range(1, checkpoints[-1] + 1):
        renderer.renderFrame()
        if update_event in profiler.events:
            update_frames.append(spp)
        if reset_event in profiler.events:
            reset_frames.append(spp)
        if spp in checkpoints:
            images.append(read_rgb(graph))
        if spp in (1, 5, checkpoints[-1]):
            diagnostics[str(spp)] = guide_stats(graph)
    renderer.renderFrame()
    events = profiler.end_capture()["events"]
    profiler.enabled = False

    timings = []
    for component in ("VBufferRT", "Tracer", "AccumulatePass"):
        samples = np.asarray(events[f"{scope}/{component}/gpu_time"]["records"], dtype=float)
        if samples.size != checkpoints[-1] or not np.all(samples > 0):
            raise ValueError(f"{graph.name}/{component} GPU 时间记录不完整")
        timings.append(samples)
    frame_ms = sum(timings)
    enabled = props["enableGuiding"]
    expected = list(range(max(1, SETTINGS["warmupFrames"]), checkpoints[-1] + 1, SETTINGS["updateInterval"])) if enabled else []
    if update_frames != expected or reset_frames != ([1] if enabled else []):
        raise AssertionError(f"缓存更新/重置帧异常：{graph.name}: {update_frames}, {reset_frames}")
    if diagnostics["1"]["active_pixel_fraction"] != 0:
        raise AssertionError("首帧使用了未清空的缓存")
    if enabled and diagnostics[str(checkpoints[-1])]["active_pixel_fraction"] <= 0:
        raise AssertionError("缓存没有实际参与采样")
    return images, {
        "cumulative_gpu_ms": np.cumsum(frame_ms)[np.asarray(checkpoints) - 1].tolist(),
        "update_frames": update_frames,
        "reset_frames": reset_frames,
        "diagnostics": diagnostics,
    }


def benchmark_scene(renderer, name, path, checkpoints, trials, reference_spp, prefix):
    renderer.loadScene(str(path))
    validation = baseline_check(renderer) if name == "CornellBox" else {}
    cases = {label: make_graph(label, dict(SETTINGS, **props)) for label, props in METHODS.items()}
    for label, graph in cases.items():
        renderer.addGraph(graph)
        renderer.setActiveGraph(graph)
        render_frames(renderer, SETTINGS["warmupFrames"] + 1)

    print(f"{name} 计算 {reference_spp} spp 参考图", flush=True)
    reference, noise = render_reference(renderer, cases["BSDF"], reference_spp)
    reference_luminance = mean_luminance(reference)
    results = {label: [] for label in cases}
    samples = {"reference": reference}
    means = {label: np.zeros_like(reference, dtype=np.float64) for label in cases}
    for trial in range(trials):
        order = list(cases)
        order = order[trial % len(order):] + order[:trial % len(order)]
        for label in order:
            images, timing = capture_trial(renderer, cases[label], METHODS[label], trial, checkpoints)
            results[label].append({
                **timing,
                "mse": [mse(image, reference) for image in images],
                "relative_luminance_error": mean_luminance(images[-1]) / reference_luminance - 1,
            })
            means[label] += images[-1] / trials
            if trial == 0:
                for spp in (16, checkpoints[-1]):
                    samples[f"{label}_{spp}spp"] = images[checkpoints.index(spp)]
        print(f"{name} 完成第 {trial + 1}/{trials} 组", flush=True)

    summary = {}
    for label, measurements in results.items():
        errors = np.asarray([trial["mse"] for trial in measurements])
        summary[label] = {
            "mean_mse": np.mean(errors, axis=0).tolist(),
            "mse_standard_error": (np.std(errors, axis=0, ddof=1) / np.sqrt(trials)).tolist(),
            "median_cumulative_gpu_ms": np.median([trial["cumulative_gpu_ms"] for trial in measurements], axis=0).tolist(),
            "mean_relative_luminance_error": float(np.mean([trial["relative_luminance_error"] for trial in measurements])),
            "ensemble_mse": mse(means[label], reference),
            "trials": measurements,
        }
        samples[f"{label}_ensemble"] = means[label].astype(np.float32)
    shared_start = max(data["median_cumulative_gpu_ms"][0] for data in summary.values())
    shared_end = min(data["median_cumulative_gpu_ms"][-1] for data in summary.values())
    anchors = [checkpoints.index(spp) for spp in (16, 64, checkpoints[-1]) if spp in checkpoints]
    budgets = sorted(set(np.clip(np.asarray(summary["BSDF"]["median_cumulative_gpu_ms"])[anchors], shared_start, shared_end).tolist()))
    for data in summary.values():
        data["equal_time_mse"] = np.exp(np.interp(np.log(budgets), np.log(data["median_cumulative_gpu_ms"]),
                                                  np.log(data["mean_mse"]))).tolist()
    for graph in cases.values():
        renderer.removeGraph(graph)
    np.savez_compressed(OUTPUT_DIR / f"{prefix}{name}.npz", **samples)
    return {
        "resolution": [reference.shape[1], reference.shape[0]],
        "reference_spp": reference_spp,
        "reference_mse_estimate": noise,
        "reference_luminance": reference_luminance,
        "baseline_validation": validation,
        "equal_time_budgets_ms": budgets,
        "modes": summary,
    }


def run(renderer):
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    smoke = os.environ.get("SC_SMOKE") == "1"
    checkpoints = (1, 4, 8, 16) if smoke else CHECKPOINTS
    max_spp = int(os.environ.get("SC_MAX_SPP", checkpoints[-1]))
    checkpoints = tuple(spp for spp in checkpoints if spp <= max_spp)
    trials = int(os.environ.get("SC_TRIALS", 2 if smoke else TRIALS))
    reference_spp = int(os.environ.get("SC_REFERENCE_SPP", 64 if smoke else REFERENCE_SPP))
    if trials < 2 or reference_spp < 2 or reference_spp % 2 or 16 not in checkpoints:
        raise ValueError("至少需要两轮、偶数参考 spp，以及包含 16 spp 的检查点")
    scenes = {
        "CornellBox": ROOT / "resource" / "CornellBox" / "CornellBox.pyscene",
        "IndirectRoom": ROOT / "scripts" / "my_graph" / "scenes" / "SignificanceCacheIndirect.pyscene",
        "FocusedRoom": ROOT / "scripts" / "my_graph" / "scenes" / "SignificanceCacheFocused.pyscene",
    }
    selected = os.environ.get("SC_SCENE")
    if selected:
        scenes = {selected: scenes[selected]}
    elif smoke:
        scenes = {"CornellBox": scenes["CornellBox"]}
    report = {
        "settings": SETTINGS,
        "methods": METHODS,
        "checkpoints_spp": checkpoints,
        "reference_spp": reference_spp,
        "trials": trials,
        "gpu_time_components": ["VBufferRT", "Tracer (including reset and updates)", "AccumulatePass"],
        "equal_time_method": "log-log interpolation, no extrapolation",
        "scenes": {},
    }
    prefix = os.environ.get("SC_OUTPUT_PREFIX", "smoke-" if smoke else "")
    result = OUTPUT_DIR / f"{prefix}results.json"
    for name, path in scenes.items():
        scene_reference_spp = max(reference_spp, int(os.environ.get("SC_FOCUSED_REFERENCE_SPP", "65536"))) if name == "FocusedRoom" and not smoke else reference_spp
        report["scenes"][name] = benchmark_scene(renderer, name, path, checkpoints, trials, scene_reference_spp, prefix)
        result.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"结果已写入 {result}", flush=True)


if "m" in globals():
    try:
        run(m)
    except Exception:
        traceback.print_exc()
        exit_mogwai(1)
    else:
        exit_mogwai()
