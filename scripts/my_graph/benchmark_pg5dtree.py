"""用 Mogwai 无窗口比较 PG5DTree 的四种模式，并保存线性 RGB 误差和 GPU 用时。"""

import json
import os
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[2]
OUTPUT_DIR = ROOT / "build" / "pg5dtree-experiment"
OUTPUT = "AccumulatePass.output"
CHECKPOINTS = (1, 4, 8, 12, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128)
TIME_ANCHORS = (16, 32, 64)
REFERENCE_FRAMES = 4096
DIRECT_REFERENCE_FRAMES = 768
TRIALS = 8
MAX_BOUNCES = 6
LUMINANCE = np.array([0.2126, 0.7152, 0.0722])
METHODS = {
    "baseline": (False, False),
    "guiding": (True, False),
    "control_variate": (False, True),
    "both": (True, True),
}
SETTINGS = {
    "guideMix": 0.5,
    "cvStrength": 0.5,
    "minRoughness": 0.25,
    "gridResolution": 16,
    "directionResolution": [8, 4],
    "warmupFrames": 8,
    "updateInterval": 4,
    "minCellSamples": 32,
}


def write_indirect_scene():
    path = OUTPUT_DIR / "IndirectRoom.pyscene"
    path.write_text(
        """from falcor import *

white = StandardMaterial("White")
white.baseColor = float4(0.78, 0.78, 0.78, 1)
white.roughness = 0.7
red = StandardMaterial("Red")
red.baseColor = float4(0.7, 0.15, 0.1, 1)
red.roughness = 0.7
shade = StandardMaterial("Shade")
shade.baseColor = float4(0.15, 0.15, 0.15, 1)
shade.roughness = 0.8
light = StandardMaterial("Upward emitter")
light.emissiveColor = float3(1, 0.85, 0.67)
light.emissiveFactor = 100.0

quad = TriangleMesh.createQuad()

def panel(name, material, scale, position, rotation=(0, 0, 0)):
    transform = Transform(scaling=float3(*scale), translation=float3(*position), rotationEulerDeg=float3(*rotation))
    sceneBuilder.addMeshInstance(sceneBuilder.addNode(name, transform), sceneBuilder.addTriangleMesh(quad, material))

panel("Floor", white, (2, 1, 2), (0, 0, 0))
panel("Ceiling", white, (2, 1, 2), (0, 2, 0), (180, 0, 0))
panel("Back", white, (2, 1, 2), (0, 1, -1), (90, 0, 0))
panel("Left", red, (2, 1, 2), (-1, 1, 0), (0, 0, -90))
panel("Right", white, (2, 1, 2), (1, 1, 0), (0, 0, 90))
panel("Emitter", light, (0.23, 1, 0.23), (0.32, 1.65, -0.43))

transform = Transform(scaling=float3(0.72, 0.06, 0.72), translation=float3(0.32, 1.60, -0.43))
sceneBuilder.addMeshInstance(
    sceneBuilder.addNode("Occluding shade", transform),
    sceneBuilder.addTriangleMesh(TriangleMesh.createCube(), shade),
)

camera = Camera("Indirect room")
camera.position = float3(0, 0.82, 3.42)
camera.target = float3(0, 0.53, -0.16)
camera.up = float3(0, 1, 0)
camera.focalLength = 50.0
sceneBuilder.addCamera(camera)
sceneBuilder.selectedCamera = camera
""",
        encoding="utf-8",
    )
    return path


def make_graph(name, properties, pass_type="PG5DTree"):
    from falcor import RenderGraph

    graph = RenderGraph(name)
    vbuffer = f"{name}VBuffer"
    tracer = f"{name}Tracer"
    graph.create_pass(vbuffer, "VBufferRT", {"samplePattern": "Center", "adjustShadingNormals": False})
    graph.create_pass(tracer, pass_type, properties)
    graph.create_pass("AccumulatePass", "AccumulatePass", {"enabled": True, "precisionMode": "SingleCompensated"})
    graph.add_edge(f"{vbuffer}.vbuffer", f"{tracer}.vbuffer")
    graph.add_edge(f"{vbuffer}.viewW", f"{tracer}.viewW")
    graph.add_edge(f"{tracer}.color", "AccumulatePass.input")
    graph.mark_output(OUTPUT)
    return graph, vbuffer, tracer


def render_frames(renderer, count):
    for _ in range(count):
        renderer.renderFrame()


def read_rgb(graph):
    image = graph.get_output(OUTPUT).to_numpy()[..., :3].copy()
    if not np.isfinite(image).all():
        raise ValueError(f"{graph.name} 的输出包含非有限值")
    return image


def mse(a, b):
    error = np.subtract(a, b, dtype=np.float64)
    return float(np.mean(np.square(error)))


def mean_luminance(image):
    return float(np.mean(image, axis=(0, 1), dtype=np.float64) @ LUMINANCE)


def render_reference(renderer, name, bounces, frames, seed):
    graph, _, _ = make_graph(name, {"maxBounces": bounces, "seed": seed})
    renderer.addGraph(graph)
    renderer.setActiveGraph(graph)
    render_frames(renderer, frames // 2)
    first = read_rgb(graph)
    graph.get_pass("AccumulatePass").reset()
    render_frames(renderer, frames // 2)
    second = read_rgb(graph)
    renderer.removeGraph(graph)
    return 0.5 * (first + second), mse(first, second) / 4


def prepare_case(renderer, name, properties):
    case = make_graph(name, dict(properties, seed=0x30000000))
    graph, _, _ = case
    renderer.addGraph(graph)
    renderer.setActiveGraph(graph)
    cache_enabled = properties["enableGuiding"] or properties["enableControlVariate"]
    # 编译射线和缓存更新管线，并让两者在开始计时前执行。
    render_frames(renderer, max(1, properties["warmupFrames"]) + 1 if cache_enabled else 2)
    return case


def capture_trial(renderer, case, properties, seed, checkpoints):
    graph, vbuffer, tracer = case
    renderer.setActiveGraph(graph)
    graph.get_pass(tracer).set_properties({"seed": seed})
    graph.get_pass("AccumulatePass").reset()
    profiler = renderer.profiler
    scope = "/onFrameRender/RenderGraphExe::execute()"
    update_event = f"{scope}/{tracer}/UpdateRadianceCache/gpu_time"
    cache_enabled = properties["enableGuiding"] or properties["enableControlVariate"]
    first_update = max(1, properties["warmupFrames"])
    update_interval = properties["updateInterval"]
    update_frames = 0
    profiler.start_capture(checkpoints[-1] + 1)
    images = []
    for spp in range(1, checkpoints[-1] + 1):
        renderer.renderFrame()
        if spp in checkpoints:
            images.append(read_rgb(graph))
        if cache_enabled and spp >= first_update and (spp - first_update) % update_interval == 0:
            if update_event not in profiler.events:
                raise ValueError(f"{graph.name} 第 {spp} 帧未发布缓存")
            update_frames += 1
    # GPU 时间戳在后一帧发布。Profiler 自身跳过第一帧的占位记录。
    renderer.renderFrame()
    events = profiler.end_capture()["events"]
    profiler.enabled = False

    vbuffer_ms = np.asarray(events[f"{scope}/{vbuffer}/gpu_time"]["records"])
    tracer_ms = np.asarray(events[f"{scope}/{tracer}/gpu_time"]["records"])
    frame_ms = vbuffer_ms + tracer_ms
    if frame_ms.size != checkpoints[-1] or not np.all(frame_ms > 0):
        raise ValueError(f"{graph.name} 的帧计时不完整")
    return images, np.cumsum(frame_ms)[np.asarray(checkpoints) - 1], update_frames


def compare_baseline(renderer):
    verification = {}
    for bounces in (0, MAX_BOUNCES):
        props = {"maxBounces": bounces}
        original, _, _ = make_graph("Original", props, "MyPathTracer")
        cloned, _, _ = make_graph(
            "Cloned", dict(props, enableGuiding=bounces == 0, enableControlVariate=bounces == 0)
        )
        images = []
        for graph in (original, cloned):
            renderer.addGraph(graph)
            renderer.setActiveGraph(graph)
            render_frames(renderer, 12)
            images.append(read_rgb(graph))
            renderer.removeGraph(graph)
        error = mse(*images)
        if not np.allclose(*images, rtol=1e-6, atol=1e-6):
            raise AssertionError(f"{bounces} 次反弹的原版和 PG5DTree 对照输出不同，MSE {error:.6g}")
        max_error = float(np.max(np.abs(images[0] - images[1])))
        verification[bounces] = {"mse": error, "max_abs_error": max_error}
        print(f"基线验证通过，{bounces} 次反弹 MSE {error:.6g}，最大绝对误差 {max_error:.6g}", flush=True)
    return verification


def summarize_timing(results):
    median_curves = {
        name: np.median([trial["gpu_ms"] for trial in method["trials"]], axis=0)
        for name, method in results.items()
    }
    baseline = median_curves["baseline"]
    nominal_budgets = [baseline[CHECKPOINTS.index(spp)] for spp in TIME_ANCHORS]
    common_start = float(max(curve[0] for curve in median_curves.values()))
    common_end = float(min(curve[-1] for curve in median_curves.values()))
    time_budgets = np.unique(np.clip(nominal_budgets, common_start, common_end)).tolist() if common_start <= common_end else []
    for name, method in results.items():
        curve = median_curves[name]
        method["median_gpu_ms_per_frame"] = float(curve[-1] / CHECKPOINTS[-1])
        method["equal_time_mse"] = np.exp(
            np.interp(np.log(time_budgets), np.log(curve), np.log(method["mean_mse"]))
        ).tolist()
    return {"shared_gpu_time_range_ms": [common_start, common_end], "equal_time_budgets_ms": time_budgets}


def benchmark_scene(renderer, scene_name, path):
    renderer.loadScene(str(path))
    print(f"{scene_name} 开始计算参考图", flush=True)
    target, target_noise = render_reference(renderer, "Reference", MAX_BOUNCES, REFERENCE_FRAMES, 0x10000000)
    direct, direct_noise = render_reference(renderer, "Direct", 0, DIRECT_REFERENCE_FRAMES, 0x20000000)
    target_luminance = mean_luminance(target)
    measurements = {name: [] for name in METHODS}
    image_sums = {name: np.zeros_like(target, dtype=np.float64) for name in METHODS}
    configs = {
        name: dict(SETTINGS, maxBounces=MAX_BOUNCES, enableGuiding=guiding, enableControlVariate=cv)
        for name, (guiding, cv) in METHODS.items()
    }
    cases = {name: prepare_case(renderer, name, config) for name, config in configs.items()}
    mode_names = list(METHODS)
    for trial in range(TRIALS):
        # 同次试验共享采样种子，各模式从空缓存起跑，并轮换执行顺序。
        for name in mode_names[trial % len(METHODS):] + mode_names[:trial % len(METHODS)]:
            images, elapsed, update_frames = capture_trial(
                renderer, cases[name], configs[name], (trial + 1) << 16, CHECKPOINTS
            )
            measurements[name].append(
                {
                    "mse": [mse(image, target) for image in images],
                    "gpu_ms": elapsed.tolist(),
                    "last_mean_luminance": mean_luminance(images[-1]),
                    "cache_update_frames": update_frames,
                }
            )
            image_sums[name] += images[-1]
        print(f"{scene_name} 完成第 {trial + 1}/{TRIALS} 次四组对照", flush=True)

    for graph, _, _ in cases.values():
        renderer.removeGraph(graph)

    results = {}
    for name, trials in measurements.items():
        errors = np.asarray([trial["mse"] for trial in trials])
        luminance_bias = np.asarray([trial["last_mean_luminance"] / target_luminance - 1 for trial in trials])
        results[name] = {
            "mean_mse": np.mean(errors, axis=0).tolist(),
            "mse_standard_error": (np.std(errors, axis=0, ddof=1) / np.sqrt(TRIALS)).tolist(),
            "mean_image_mse": mse(image_sums[name] / TRIALS, target),
            "relative_mean_luminance_error": float(np.mean(luminance_bias)),
            "relative_luminance_standard_error": float(np.std(luminance_bias, ddof=1) / np.sqrt(TRIALS)),
            "trials": trials,
        }
    np.savez_compressed(OUTPUT_DIR / f"{scene_name}.npz", target=target, direct=direct, **{
        name: image / TRIALS for name, image in image_sums.items()
    })
    return {
        "resolution": [target.shape[1], target.shape[0]],
        "reference_spp": REFERENCE_FRAMES,
        "reference_noise_mse": target_noise,
        "direct_reference_spp": DIRECT_REFERENCE_FRAMES,
        "direct_reference_noise_mse": direct_noise,
        "mean_luminance": target_luminance,
        "indirect_luminance_fraction": 1 - mean_luminance(direct) / target_luminance,
        **summarize_timing(results),
        "methods": results,
    }


def run(renderer):
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    cornell = ROOT / "resource" / "CornellBox" / "CornellBox.pyscene"
    smoke = os.environ.get("PG5DTREE_SMOKE") == "1"
    renderer.loadScene(str(cornell))
    verification = compare_baseline(renderer)
    if smoke:
        for name, (guiding, cv) in METHODS.items():
            props = dict(SETTINGS, enableGuiding=guiding, enableControlVariate=cv)
            case = prepare_case(renderer, name, props)
            images, elapsed, updates = capture_trial(renderer, case, props, 0, (1, 8, 12, 16))
            renderer.removeGraph(case[0])
            print(f"{name} 16 spp，平均亮度 {mean_luminance(images[-1]):.6g}，GPU {elapsed[-1]:.3f} ms，更新 {updates} 次", flush=True)
        return

    report = {
        "checkpoints": CHECKPOINTS, "trials": TRIALS, "max_bounces": MAX_BOUNCES,
        "cache": SETTINGS, "baseline_verification": verification, "scenes": {},
    }
    scenes = {"CornellBox": cornell, "IndirectRoom": write_indirect_scene()}
    selected = os.environ.get("PG5DTREE_SCENE")
    result_path = OUTPUT_DIR / (f"results-{selected}.json" if selected else "results.json")
    if selected:
        scenes = {selected: scenes[selected]}
    for scene_name, path in scenes.items():
        report["scenes"][scene_name] = benchmark_scene(renderer, scene_name, path)
        result_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"结果已保存到 {result_path}", flush=True)


if "m" in globals():
    run(m)
    exit()
