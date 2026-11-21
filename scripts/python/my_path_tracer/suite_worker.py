# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
"""在独立 headless 进程中运行一个场景，释放场景间的 GPU 资源。"""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

import falcor
import numpy as np

if __package__:
    from .graphs import create_rtxdi_comparison_graph
    from .scenes import REPOSITORY, prepare_mitsuba_scene, scene_import_script
    from .validation import RADIANCE_OUTPUTS, read_checked_radiance
else:
    from graphs import create_rtxdi_comparison_graph
    from scenes import REPOSITORY, prepare_mitsuba_scene, scene_import_script
    from validation import RADIANCE_OUTPUTS, read_checked_radiance


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def vector(value):
    return [float(value.x), float(value.y), float(value.z)]


def source_hashes():
    roots = [REPOSITORY / path for path in (
        "Source/RenderPasses/MyPathTracer", "Source/RenderPasses/RTXDIPass", "Source/RenderPasses/PathTracer",
        "Source/Falcor/Rendering/RTXDI", "scripts/python/my_path_tracer",
    )]
    files = [path for root in roots for path in root.rglob("*")
             if path.is_file() and "__pycache__" not in path.parts]
    return {path.relative_to(REPOSITORY).as_posix(): sha256(path) for path in sorted(files)}


def run_case(config_path):
    config = json.loads(config_path.read_text(encoding="utf-8"))
    output = config_path.parent
    write_json(output / "status.json", {"status": "running", "phase": "loading_scene"})
    try:
        api = falcor.DeviceType.D3D12 if config["device_type"] == "d3d12" else falcor.DeviceType.Vulkan
        width, height = config["resolution"]
        testbed = falcor.Testbed(width=width, height=height, create_window=False, device_type=api, gpu=config["gpu"])
        testbed.show_ui = False
        testbed.clock.time = 0
        testbed.clock.pause()
        graph = create_rtxdi_comparison_graph(
            testbed, config["my_properties"], config["rtxdi_options"], output / "native_difference.csv",
            baseline_renderer=config.get("baseline_renderer", "rtxdi-pass"),
        )
        testbed.render_graph = graph
        source_scene = Path(config["resolved_scene"])
        import_path = prepare_mitsuba_scene(source_scene, output) if source_scene.suffix.lower() == ".xml" else source_scene
        compatibility = json.loads((output / "mitsuba_compatibility.json").read_text()) if import_path != source_scene else {}
        script = scene_import_script(config["scene"], import_path, compatibility.get("double_sided_ids", []))
        (output / "scene_import.pyscene").write_text(script, encoding="utf-8")
        testbed.load_scene_from_string(script)
        scene = testbed.scene
        scene.animated = False
        camera = scene.camera
        camera.apertureRadius = 0.0
        camera.aspectRatio = width / height
        imported_clip = {"near_plane": float(camera.nearPlane), "far_plane": float(camera.farPlane)}
        clipping = config["scene"].get("camera_clip", {})
        if clipping:
            camera.nearPlane = clipping["near_plane"]
            camera.farPlane = clipping["far_plane"]
        camera_state = {
            "position": vector(camera.position), "target": vector(camera.target), "up": vector(camera.up),
            "focal_length": float(camera.focalLength), "aperture_radius": float(camera.apertureRadius),
            "aspect_ratio": float(camera.aspectRatio), "source": "imported selected camera (or Falcor automatic camera)",
            "near_plane": float(camera.nearPlane), "far_plane": float(camera.farPlane),
            "imported_clipping": imported_clip,
        }
        # Scene's first forced update can replace an imported animated camera
        # with its bind pose when animation is disabled. Freeze the imported
        # world pose after that update, before all warmup/measurement frames.
        write_json(output / "status.json", {"status": "running", "phase": "initializing_scene"})
        testbed.frame()
        read_checked_radiance(graph, output, "initialization", 0)
        for name in ("position", "target", "up"):
            setattr(camera, name, falcor.float3(*camera_state[name]))
        write_json(output / "scene_state.json", {
            "camera": camera_state, "scene_stats": scene.stats,
            "bounds": {"min": vector(scene.bounds.min_point), "max": vector(scene.bounds.max_point)},
            "initialization_frames": 1, "camera_pose_restored_after_initial_scene_update": True,
        })
        testbed.profiler.enabled = True
        write_json(output / "status.json", {"status": "running", "phase": "warmup"})
        for frame in range(config["warmup_frames"]):
            testbed.frame()
            read_checked_radiance(graph, output, "warmup", 1 + frame)
        for name in ("position", "target", "up"):
            if not np.allclose(vector(getattr(camera, name)), camera_state[name], rtol=0, atol=1e-6):
                raise RuntimeError("相机在预热中发生变化，不能进行固定视角对比: " + name)
        for name in ("BaselineAccumulate", "MyAccumulate"):
            graph.get_pass(name).reset()
        if config.get("baseline_renderer") == "path-tracer":
            effective = graph.get_pass("RTXDIBaseline").properties
            expected = {"useRTXDI": True, "useReSTIRPT": False, "samplesPerPixel": 1,
                        "maxSurfaceBounces": 0, "maxDiffuseBounces": 0, "maxSpecularBounces": 0, "maxTransmissionBounces": 0}
            if any(effective.get(key) != value for key, value in expected.items()):
                raise RuntimeError("原生 RTXDI 直接光照配置未生效: " + str(effective))

        frames = config["frames"]
        testbed.profiler.start_capture(frames + 1)
        rows = []
        for frame in range(frames):
            testbed.frame()
            images = read_checked_radiance(graph, output, "measurement", 1 + config["warmup_frames"] + frame, frame)
            baseline, candidate = images["baseline"], images["candidate"]
            delta = candidate[..., :3].astype(np.float64) - baseline[..., :3].astype(np.float64)
            rows.append({
                "measurement_frame": frame, "renderer_frame": 1 + config["warmup_frames"] + frame,
                "accumulated_frames": frame + 1,
                "mse_to_baseline_rgb": float(np.mean(delta ** 2)),
                "mae_to_baseline_rgb": float(np.mean(np.abs(delta))),
                "max_abs_to_baseline_rgb": float(np.max(np.abs(delta))),
                "exact_equal": bool(np.array_equal(candidate, baseline)),
            })
        # Save exactly the requested measurement interval before draining timestamps.
        np.savez_compressed(output / "linear_images.npz", baseline=baseline, candidate=candidate)
        if not np.any(np.abs(baseline[..., :3]) > 1e-12):
            raise RuntimeError("RTXDI baseline 全黑：需要检查相机、裁剪范围与光照，不能作为已验证场景")
        for index, name in enumerate(("baseline", "my", "squared_difference")):
            testbed.capture_output(str(output / (name + ".exr")), index)
        testbed.frame()
        read_checked_radiance(graph, output, "profiler_drain", 1 + config["warmup_frames"] + frames)
        capture = testbed.profiler.end_capture()
        if capture["frame_count"] != frames:
            raise RuntimeError("原生 Profiler 返回的帧数与测量区间不一致")
        write_json(output / "profiler.json", capture)
        write_json(output / "metrics.json", rows)
        with (output / "metrics.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

        runtime = Path(falcor.__file__).resolve().parents[2]
        compare = runtime / ("ImageCompare.exe" if sys.platform == "win32" else "ImageCompare")
        result = subprocess.run(
            [str(compare), "-m", "mse", "-t", "0", str(output / "baseline.exr"), str(output / "my.exr")],
            capture_output=True, text=True, check=False,
        )
        (output / "image_compare.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
        # Different algorithms are expected to differ: exit code 1 is a measurement,
        # not a failed render or a comparison against ground truth.
        if result.returncode not in (0, 1):
            raise RuntimeError("原生 ImageCompare 执行失败: " + result.stderr)
        native_mse = float(result.stdout.strip())
        if not np.isfinite(native_mse):
            raise RuntimeError("原生 ImageCompare 指标无效")
        timing = {}
        for name in ("MyPathTracer", "RTXDIBaseline", "VBufferRT"):
            lanes = [lane for key, lane in capture["events"].items() if key.endswith("/" + name + "/gpu_time")]
            if len(lanes) != 1:
                raise RuntimeError("缺少唯一的原生 GPU 计时 scope: " + name)
            timing[name] = lanes[0]["stats"]
        scene_path = Path(config["resolved_scene"])
        provenance = {
            "falcor_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPOSITORY, text=True).strip(),
            "source_sha256": source_hashes(), "scene_entry_sha256": sha256(scene_path),
            "imported_scene_sha256": sha256(import_path), "import_compatibility": compatibility,
            "scene_catalog": config["scene"], "camera": camera_state,
            "device": {"name": testbed.device.info.adapter_name, "api": testbed.device.info.api_name},
            "python": sys.version, "numpy": np.__version__, "platform": platform.platform(),
            "baseline": config["baseline"], "baseline_renderer": config.get("baseline_renderer", "rtxdi-pass"),
            "effective_properties": {name: graph.get_pass(name).properties for name in ("VBufferRT", "MyPathTracer", "RTXDIBaseline")},
            "scene_stats": scene.stats, "lighting": repr(scene.renderSettings),
            "rng": "Unmodified native pixel/frame and RTXDI frame-index generators; no configurable independent seed is claimed",
            "profiler_frame_count": capture["frame_count"],
            "initialization_frames": 1,
            "radiance_validation": {"outputs": RADIANCE_OUTPUTS, "checked_frames": 2 + config["warmup_frames"] + frames,
                                   "nonfinite_components": 0, "includes_initialization_warmup_measurement_and_drain": True},
        }
        try:
            driver = subprocess.run(["nvidia-smi", "--query-gpu=name,driver_version", "--format=csv,noheader"],
                                    capture_output=True, text=True, timeout=5, check=False)
            provenance["nvidia_driver_inventory"] = driver.stdout.strip() if driver.returncode == 0 else None
        except (OSError, subprocess.TimeoutExpired):
            provenance["nvidia_driver_inventory"] = None
        write_json(output / "provenance.json", provenance)
        baseline_luminance = baseline[..., :3] @ np.array([0.2126, 0.7152, 0.0722])
        lit_fraction = float(np.mean(baseline_luminance > max(1e-8, float(baseline_luminance.max()) * 1e-6)))
        summary = {
            "status": "completed", "scene_id": config["scene"]["id"], "frames": frames,
            "baseline_pass": "RTXDIBaseline", "baseline_renderer": config.get("baseline_renderer", "rtxdi-pass"),
            "baseline_label": "PathTracer + RTXDI (DI only)" if config.get("baseline_renderer") == "path-tracer" else "RTXDIPass",
            "metrics": rows[-1], "native_image_compare_mse_rgb": native_mse,
            "gpu_time_ms": timing,
            "metric_interpretation": "Distance to stock RTXDI baseline, not error against ground truth",
            "timing_interpretation": "Native GPU pass scopes in a shared comparison graph; not isolated equal-time performance",
            "baseline_luminance_mean": float(np.mean(baseline[..., :3] @ np.array([0.2126, 0.7152, 0.0722]))),
            "my_luminance_mean": float(np.mean(candidate[..., :3] @ np.array([0.2126, 0.7152, 0.0722]))),
            "radiance_validation": provenance["radiance_validation"],
            "baseline_lit_pixel_fraction": lit_fraction,
            "warnings": ["当前直接光照配置仅照亮少量像素；该场景主要用于间接光照研究，不能将此图解释为 GI 质量对比。"] if lit_fraction < 0.01 else [],
        }
        write_json(output / "summary.json", summary)
        write_json(output / "status.json", {"status": "completed", "frames": frames})
        print(json.dumps({"scene": config["scene"]["id"], "status": "completed", "metrics": rows[-1]}, ensure_ascii=False))
        return 0
    except Exception as error:
        write_json(output / "status.json", {"status": "failed", "error": str(error)})
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    return run_case(parser.parse_args().config.resolve())


if __name__ == "__main__":
    raise SystemExit(main())
