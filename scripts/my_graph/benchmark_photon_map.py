"""在 Mogwai 无窗口模式下测量光子引导的线性 RGB 误差和 GPU 时间。"""

import json
import os
import traceback
from pathlib import Path

import numpy as np

from falcor import RenderGraph, exit as exit_mogwai


ROOT = Path(__file__).resolve().parents[2]
OUTPUT_DIR = ROOT / "build" / "photon-map-experiment"
TRACER = "PhotonMapPathTracer"
OUTPUT = "AccumulatePass.output"
CHECKPOINTS = (1, 4, 8, 16, 24, 32, 48, 64, 96, 128)
REFERENCE_SPP = 2048
TRIALS = 6
LUMINANCE = np.array([0.2126, 0.7152, 0.0722])
SETTINGS = {
    "maxBounces": 6,
    "guidedBounces": 1,
    "photonPaths": 131072,
    "photonBounces": 4,
    "gatherRadius": 0.04,
    "minPhotons": 4,
    "guideMix": 0.5,
}


def write_indirect_scene():
    path = OUTPUT_DIR / "UpwardEmitter.pyscene"
    path.write_text(
        """from falcor import *

def diffuse(name, rgb):
    material = StandardMaterial(name)
    material.baseColor = float4(*rgb, 1)
    material.roughness = 1
    return material

white = diffuse("White", (0.8, 0.8, 0.8))
red = diffuse("Red", (0.73, 0.12, 0.08))
green = diffuse("Green", (0.12, 0.65, 0.17))
shade = diffuse("Shade", (0.08, 0.08, 0.08))
lamp = diffuse("Upward emitter", (0.0, 0.0, 0.0))
lamp.emissiveColor = float3(1, 0.87, 0.68)
lamp.emissiveFactor = 100
quad = TriangleMesh.createQuad()

def panel(name, material, scale, position, rotation=(0, 0, 0)):
    transform = Transform(scaling=float3(*scale), translation=float3(*position), rotationEulerDeg=float3(*rotation))
    node = sceneBuilder.addNode(name, transform)
    sceneBuilder.addMeshInstance(node, sceneBuilder.addTriangleMesh(quad, material))

panel("Floor", white, (2, 1, 2), (0, 0, 0))
panel("Ceiling", white, (2, 1, 2), (0, 2, 0), (180, 0, 0))
panel("Back", white, (2, 1, 2), (0, 1, -1), (90, 0, 0))
panel("Left", red, (2, 1, 2), (-1, 1, 0), (0, 0, -90))
panel("Right", green, (2, 1, 2), (1, 1, 0), (0, 0, 90))
panel("Emitter", lamp, (0.26, 1, 0.26), (0.36, 1.81, -0.52))
panel("Lamp backing", shade, (0.38, 1, 0.38), (0.36, 1.80, -0.52), (180, 0, 0))

camera = Camera("Indirect lighting room")
camera.position = float3(0, 0.88, 3.35)
camera.target = float3(0, 0.72, -0.24)
camera.up = float3(0, 1, 0)
camera.focalLength = 50.0
sceneBuilder.addCamera(camera)
sceneBuilder.selectedCamera = camera
""",
        encoding="utf-8",
    )
    return path


def make_graph(name, guiding):
    graph = RenderGraph(name)
    graph.create_pass("VBufferRT", "VBufferRT", {"samplePattern": "Center", "adjustShadingNormals": False})
    graph.create_pass(TRACER, TRACER, dict(SETTINGS, enableGuiding=guiding))
    graph.create_pass("AccumulatePass", "AccumulatePass", {"enabled": True, "precisionMode": "SingleCompensated"})
    graph.add_edge("VBufferRT.vbuffer", f"{TRACER}.vbuffer")
    graph.add_edge("VBufferRT.viewW", f"{TRACER}.viewW")
    graph.add_edge(f"{TRACER}.color", "AccumulatePass.input")
    graph.mark_output(OUTPUT)
    return graph


def render_frames(renderer, frames):
    for _ in range(frames):
        renderer.renderFrame()


def read_rgb(graph):
    rgb = graph.get_output(OUTPUT).to_numpy()[..., :3].copy()
    if not np.isfinite(rgb).all():
        raise ValueError(f"{graph.name} 输出包含非有限值")
    return rgb


def mse(image, reference):
    return float(np.mean(np.square(np.subtract(image, reference, dtype=np.float64))))


def mean_luminance(image):
    return float(np.mean(image, axis=(0, 1), dtype=np.float64) @ LUMINANCE)


def start_case(renderer, graph, **props):
    renderer.setActiveGraph(graph)
    graph.get_pass(TRACER).set_properties(props)
    graph.get_pass("AccumulatePass").reset()


def render_reference(renderer, graph, frames):
    halves = []
    for seed in (0x40000000, 0x50000000):
        start_case(renderer, graph, seed=seed)
        render_frames(renderer, frames // 2)
        halves.append(read_rgb(graph))
    return (halves[0] + halves[1]) * 0.5, mse(*halves) / 4


def capture_trial(renderer, graph, guiding, trial, checkpoints):
    start_case(renderer, graph, seed=(trial + 1) << 16, photonSeed=trial + 1)
    profiler = renderer.profiler
    scope = "/onFrameRender/RenderGraphExe::execute()"
    build_event = f"{scope}/{TRACER}/BuildPhotonMap/gpu_time"
    profiler.start_capture(checkpoints[-1] + 1)
    images = []
    build_frames = []
    for frame in range(1, checkpoints[-1] + 1):
        renderer.renderFrame()
        if build_event in profiler.events:
            build_frames.append(frame)
        if frame in checkpoints:
            images.append(read_rgb(graph))
    # GPU 计时在后一帧发布。
    renderer.renderFrame()
    events = profiler.end_capture()["events"]
    profiler.enabled = False

    vbuffer_ms = np.asarray(events[f"{scope}/VBufferRT/gpu_time"]["records"])
    tracer_ms = np.asarray(events[f"{scope}/{TRACER}/gpu_time"]["records"])
    if tracer_ms.size != checkpoints[-1] or not np.all(tracer_ms > 0):
        raise ValueError("逐帧 GPU 计时不完整")
    if build_frames != ([1] if guiding else []):
        raise ValueError(f"{graph.name} 的光子图构建帧异常：{build_frames}")
    frame_ms = vbuffer_ms + tracer_ms
    return images, {
        "cumulative_gpu_ms": np.cumsum(frame_ms)[np.asarray(checkpoints) - 1].tolist(),
        "steady_gpu_ms": float(np.mean(frame_ms[1:])),
        "map_build_frames": build_frames,
    }


def benchmark_scene(renderer, name, scene, checkpoints, trials, reference_spp, artifact_prefix):
    renderer.loadScene(str(scene))
    cases = {label: make_graph(label, label == "Guided") for label in ("BSDF", "Guided")}
    for graph in cases.values():
        renderer.addGraph(graph)
        renderer.setActiveGraph(graph)
        render_frames(renderer, 2)

    reference, reference_noise = render_reference(renderer, cases["BSDF"], reference_spp)
    reference_luminance = mean_luminance(reference)
    results = {label: [] for label in cases}
    samples = {"reference": reference}
    for trial in range(trials):
        order = tuple(cases) if trial % 2 == 0 else tuple(reversed(cases))
        for label in order:
            images, timing = capture_trial(renderer, cases[label], label == "Guided", trial, checkpoints)
            results[label].append({
                **timing,
                "mse": [mse(image, reference) for image in images],
                "relative_luminance_error": mean_luminance(images[-1]) / reference_luminance - 1,
            })
            if trial == 0:
                for spp in (16, checkpoints[-1]):
                    samples[f"{label}_{spp}spp"] = images[checkpoints.index(spp)]
        print(f"{name} 完成第 {trial + 1}/{trials} 组对照", flush=True)

    if np.array_equal(samples[f"BSDF_{checkpoints[-1]}spp"], samples[f"Guided_{checkpoints[-1]}spp"]):
        raise AssertionError(f"{name} 引导启用后输出无变化")
    for graph in cases.values():
        renderer.removeGraph(graph)
    np.savez_compressed(OUTPUT_DIR / f"{artifact_prefix}{name}.npz", **samples)

    summary = {}
    for label, measurements in results.items():
        errors = np.asarray([trial["mse"] for trial in measurements])
        summary[label] = {
            "mean_mse": np.mean(errors, axis=0).tolist(),
            "median_cumulative_gpu_ms": np.median([trial["cumulative_gpu_ms"] for trial in measurements], axis=0).tolist(),
            "median_steady_gpu_ms": float(np.median([trial["steady_gpu_ms"] for trial in measurements])),
            "mean_relative_luminance_error": float(np.mean([trial["relative_luminance_error"] for trial in measurements])),
            "trials": measurements,
        }
    return {
        "resolution": [reference.shape[1], reference.shape[0]],
        "reference_mse_estimate": reference_noise,
        "reference_luminance": reference_luminance,
        "modes": summary,
    }


def run(renderer):
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    smoke = os.environ.get("PHOTON_MAP_SMOKE") == "1"
    checkpoints = (1, 4, 8, 16) if smoke else CHECKPOINTS
    trials = 2 if smoke else TRIALS
    reference_spp = 32 if smoke else REFERENCE_SPP
    scenes = {"CornellBox": ROOT / "resource" / "CornellBox" / "CornellBox.pyscene"}
    if not smoke:
        scenes["UpwardEmitter"] = write_indirect_scene()
    report = {
        "settings": SETTINGS,
        "checkpoints_spp": checkpoints,
        "trials": trials,
        "reference_spp": reference_spp,
        "scenes": {},
    }
    result_path = OUTPUT_DIR / ("smoke-results.json" if smoke else "results.json")
    for name, scene in scenes.items():
        report["scenes"][name] = benchmark_scene(renderer, name, scene, checkpoints, trials, reference_spp, "smoke-" if smoke else "")
        result_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"结果已写入 {result_path}", flush=True)


if "m" in globals():
    try:
        run(m)
    except Exception:
        traceback.print_exc()
        exit_mogwai(1)
    else:
        exit_mogwai()
