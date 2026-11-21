# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
"""Compare MyPathTracer with MinimalPathTracer through Falcor's native Testbed.

Use a Python environment matching the Falcor build and its generated setpath script as described
in docs/falcor-in-python.md. No framework patches or custom render executable
are required.
"""

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import subprocess
import sys

import falcor
import numpy as np

if __package__:
    from .graphs import create_comparison_graph
else:
    from graphs import create_comparison_graph


REPOSITORY = Path(__file__).resolve().parents[3]


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")


def source_hashes():
    roots = [REPOSITORY / "Source/RenderPasses" / name for name in ("MinimalPathTracer", "MyPathTracer")]
    files = [path for root in roots for path in root.iterdir() if path.is_file()]
    files += sorted(Path(__file__).parent.glob("*.py"))
    return {path.relative_to(REPOSITORY).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(files)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", type=Path, default=REPOSITORY / "media/test_scenes/cornell_box.pyscene")
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=144)
    parser.add_argument("--frames", type=int, default=32)
    parser.add_argument("--warmup", type=int, default=8)
    parser.add_argument("--max-bounces", type=int, default=3)
    parser.add_argument("--uniform-bsdf", action="store_true")
    parser.add_argument("--indirect-only", action="store_true")
    parser.add_argument("--without-view-buffer", action="store_true")
    parser.add_argument("--device-type", choices=("d3d12", "vulkan"), default="d3d12" if sys.platform == "win32" else "vulkan")
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--atol", type=float, default=0.0, help="Per-component absolute tolerance (default: exact equality)")
    parser.add_argument("--rtol", type=float, default=0.0, help="Per-component relative tolerance")
    parser.add_argument("--image-mse-threshold", type=float, default=0.0, help="Native ImageCompare MSE threshold for exported EXRs")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if min(args.width, args.height, args.frames) < 1 or args.warmup < 2 or args.max_bounces < 0:
        parser.error("Resolution/frames must be positive, warmup >= 2, and max-bounces >= 0")
    if any(not math.isfinite(value) or value < 0 for value in (args.atol, args.rtol, args.image_mse_threshold)):
        parser.error("Comparison tolerances must be finite and non-negative")
    scene = args.scene.resolve()
    if not scene.is_file():
        parser.error("Scene does not exist: " + str(scene))
    output = args.output or REPOSITORY / "test-results/my_path_tracer" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S_%fZ")
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    properties = {
        "maxBounces": args.max_bounces,
        "computeDirect": not args.indirect_only,
        "useImportanceSampling": not args.uniform_bsdf,
    }
    config = {
        "scene": str(scene), "resolution": [args.width, args.height], "frames": args.frames,
        "warmup_frames": args.warmup, "properties": properties, "bind_view": not args.without_view_buffer,
        "device_type": args.device_type, "gpu": args.gpu,
        "tolerances": {"atol": args.atol, "rtol": args.rtol, "image_mse_threshold": args.image_mse_threshold},
        "rng": "Unmodified MinimalPathTracer pixel/frame sample generator; frame counters start together at scene load",
        "measurement_frames": [args.warmup, args.warmup + args.frames - 1],
        "exported_frame": args.warmup + args.frames,
        "transport": "Unmodified MinimalPathTracer behavior; direct + indirect according to the recorded properties",
        "timing": "Native Falcor Profiler capture after warmup; both passes execute in one graph and share VBufferRT. GPU scopes exclude CPU image readback and saving. This is an equivalence benchmark, not an isolated-performance comparison.",
    }
    write_json(output / "config.json", config)
    write_json(output / "status.json", {"status": "running"})
    try:
        device_type = falcor.DeviceType.D3D12 if args.device_type == "d3d12" else falcor.DeviceType.Vulkan
        testbed = falcor.Testbed(width=args.width, height=args.height, create_window=False, device_type=device_type, gpu=args.gpu)
        testbed.show_ui = False
        testbed.clock.time = 0
        testbed.clock.pause()
        graph = create_comparison_graph(testbed, properties, not args.without_view_buffer, output / "error_measurements.csv")
        testbed.render_graph = graph
        testbed.load_scene(str(scene))
        testbed.scene.animated = False
        testbed.profiler.enabled = True
        for _ in range(args.warmup):
            testbed.frame()

        # Profiler capture initializes its lanes on the first frame and reports
        # GPU timestamps one frame late. The extra frame drains the last sample.
        testbed.profiler.start_capture(args.frames + 1)
        rows = []
        for frame in range(args.frames + 1):
            testbed.frame()
            if frame == args.frames:
                break
            original = graph.get_output("MinimalPathTracer.color").to_numpy().copy()
            candidate = graph.get_output("MyPathTracer.color").to_numpy().copy()
            if not np.isfinite(original).all() or not np.isfinite(candidate).all():
                raise RuntimeError("Non-finite radiance invalidates the comparison")
            difference = candidate.astype(np.float64) - original.astype(np.float64)
            rows.append({
                "frame": args.warmup + frame,
                "exact_equal": bool(np.array_equal(candidate, original)),
                "within_tolerance": bool(np.allclose(candidate, original, rtol=args.rtol, atol=args.atol)),
                "max_abs_rgba": float(np.abs(difference).max()),
                "mse_rgb": float(np.mean(difference[..., :3] ** 2)),
                "mae_rgb": float(np.mean(np.abs(difference[..., :3]))),
            })
        capture = testbed.profiler.end_capture()
        write_json(output / "profiler.json", capture)
        write_json(output / "metrics.json", rows)
        with (output / "metrics.csv").open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

        # Capture native graph outputs as EXR; no tone mapper is present.
        for index, name in enumerate(("minimal", "my", "squared_difference")):
            testbed.capture_output(str(output / (name + ".exr")), index)
        runtime = Path(falcor.__file__).resolve().parents[2]
        compare = runtime / ("ImageCompare.exe" if sys.platform == "win32" else "ImageCompare")
        if not compare.is_file():
            raise RuntimeError("Build the native ImageCompare target to verify the exported EXRs")
        result = subprocess.run(
            [str(compare), "-m", "mse", "-t", str(args.image_mse_threshold), str(output / "minimal.exr"), str(output / "my.exr")],
            capture_output=True, text=True, check=False,
        )
        (output / "image_compare.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
        exact_equal = all(row["exact_equal"] for row in rows)
        passed = all(row["within_tolerance"] for row in rows) and result.returncode == 0
        provenance = {
            "falcor_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPOSITORY, text=True).strip(),
            "source_sha256": source_hashes(), "python": sys.version,
            "numpy": np.__version__, "platform": platform.platform(),
            "device": {"adapter_name": testbed.device.info.adapter_name, "api_name": testbed.device.info.api_name},
            "effective_properties": {name: graph.get_pass(name).properties for name in ("MinimalPathTracer", "MyPathTracer")},
            "scene_stats": testbed.scene.stats,
            "profiler_frame_count": capture["frame_count"],
            "image_compare_returncode": result.returncode,
        }
        write_json(output / "provenance.json", provenance)
        write_json(output / "status.json", {"status": "passed" if passed else "failed", "exact_equal": exact_equal, "compared_frames": len(rows)})
        print(json.dumps({"passed": passed, "exact_equal": exact_equal, "compared_frames": len(rows), "output": str(output)}))
        return 0 if passed else 1
    except Exception as error:
        write_json(output / "status.json", {"status": "failed", "error": str(error)})
        raise


if __name__ == "__main__":
    raise SystemExit(main())
