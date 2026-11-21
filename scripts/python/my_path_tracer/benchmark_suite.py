# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
"""一条命令逐场景对比 MyPathTracer 与 Falcor 原生 RTXDI，并生成图像报告。"""

import argparse
import copy
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys

if __package__:
    from .scenes import REPOSITORY, default_assets_root, load_catalog, resolve_scene, select_scenes
else:
    from scenes import REPOSITORY, default_assets_root, load_catalog, resolve_scene, select_scenes

PROFILES = {"smoke": (160, 90, 4, 4), "preview": (640, 360, 64, 32), "paper": (1280, 720, 256, 64)}


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")


def replot_results(source, output, selected, suite, include_html=False):
    """按当前 DI 清单重绘已有实测数据，保留原始帧数、参数与失败状态。"""
    state = json.loads((source / "suite.json").read_text(encoding="utf-8"))
    indexed = {case["id"]: case for case in state["cases"]}
    cases = [copy.deepcopy(indexed[scene["id"]]) if scene["id"] in indexed else
             {"id": scene["id"], "name": scene["name"], "status": "skipped", "reason": "历史运行中没有该场景结果"}
             for scene in selected]
    config = {**state["config"], "suite": suite, "selected_scenes": [scene["id"] for scene in selected],
              "replot_only": True, "source_results": str(source)}
    output.mkdir(parents=True, exist_ok=False)
    if __package__:
        from .report import render_suite
    else:
        from report import render_suite
    render_suite(output, cases, config, include_html=include_html, data_root=source)
    counts = {status: sum(case["status"] == status for case in cases) for status in ("completed", "failed", "skipped")}
    save(output / "config.json", config)
    save(output / "suite.json", {"status": "failed" if counts["failed"] else "completed_with_skips" if counts["skipped"] else "completed",
                                  "config": config, "cases": cases, "counts": {"selected": len(cases), **counts}})
    print(json.dumps({"mode": "replot", **counts, "data_figure": str(output / "data_summary.png"),
                      "comparison_figure": str(output / "comparison_summary.png")}, ensure_ascii=False))
    return 0


def main():
    catalog = load_catalog()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=catalog["suites"], default="paper")
    parser.add_argument("--scenes", nargs="+", help="只运行指定场景 ID")
    parser.add_argument("--list", action="store_true", help="列出场景、资产可用状态和缺失原因，不需要加载 Falcor")
    parser.add_argument("--assets-root", type=Path, default=default_assets_root())
    parser.add_argument("--profile", choices=PROFILES, default="preview")
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int)
    parser.add_argument("--frames", type=int)
    parser.add_argument("--warmup", type=int)
    parser.add_argument("--my-max-bounces", type=int, default=0, help="默认直接光照；支持使用现有 pass 参数调整")
    parser.add_argument("--rtxdi-options", type=Path, help="RTXDI options JSON；省略时使用原生默认参数")
    parser.add_argument("--baseline-renderer", choices=("path-tracer", "rtxdi-pass"), default="path-tracer",
                        help="默认使用官方 PathTracer 的 RTXDI 直接光照集成；rtxdi-pass 保留为独立示例的诊断参照")
    parser.add_argument("--device-type", choices=("d3d12", "vulkan"), default="d3d12" if sys.platform == "win32" else "vulkan")
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--timeout", type=int, default=900, help="单场景进程超时秒数")
    parser.add_argument("--require-complete", action="store_true", help="缺失场景也视为套件失败")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--html", action="store_true", help="额外生成 HTML；默认直接输出 PNG 数据与图像总图")
    parser.add_argument("--replot", type=Path, help="仅按当前 DI 清单重绘已有结果目录，不重新运行 GPU")
    args = parser.parse_args()
    try:
        selected = select_scenes(catalog, args.suite, args.scenes)
    except ValueError as error:
        parser.error(str(error))
    if args.list:
        available = 0
        for scene in selected:
            path, reason = resolve_scene(scene, args.assets_root)
            available += path is not None
            print(f"{scene['id']}: {'可用资产' if path else '不可用'} | {scene['name']}" + (" | " + reason if reason else ""))
        print(f"当前选择 {len(selected)} 个配置，资产可用 {available}，不可用 {len(selected) - available}；资产可用不代表已通过渲染验证。")
        return 0
    if args.replot:
        output = (args.output or REPOSITORY / "test-results/my_path_tracer" /
                  ("di_summary_" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S_%fZ"))).resolve()
        return replot_results(args.replot.resolve(), output, selected, args.suite, args.html)
    width, height, frames, warmup = PROFILES[args.profile]
    width = args.width if args.width is not None else width
    height = args.height if args.height is not None else height
    frames = args.frames if args.frames is not None else frames
    warmup = args.warmup if args.warmup is not None else warmup
    if min(width, height, frames, args.timeout) <= 0 or warmup < 2 or args.my_max_bounces < 0 or args.gpu < 0:
        parser.error("分辨率、帧数、超时必须为正；warmup >= 2；bounce 和 GPU 编号不能为负")
    rtxdi_options = json.loads(args.rtxdi_options.read_text(encoding="utf-8")) if args.rtxdi_options else {}
    if not isinstance(rtxdi_options, dict):
        parser.error("RTXDI options 必须是 JSON 对象")
    output = (args.output or REPOSITORY / "test-results/my_path_tracer" /
              ("suite_" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S_%fZ"))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    config = {
        "suite": args.suite, "selected_scenes": [scene["id"] for scene in selected],
        "profile": args.profile, "resolution": [width, height], "frames": frames, "warmup_frames": warmup,
        "initialization_frames": 1, "measurement_renderer_frames": [1 + warmup, warmup + frames],
        "my_properties": {"maxBounces": args.my_max_bounces, "computeDirect": True, "useImportanceSampling": True},
        "rtxdi_options": rtxdi_options, "baseline_renderer": args.baseline_renderer,
        "baseline": catalog["baseline"] if args.baseline_renderer == "path-tracer" else "Falcor 9.0 native RTXDIPass (stock RTXDI options)",
        "device_type": args.device_type, "gpu": args.gpu, "assets_root": str(args.assets_root.resolve()),
        "measurement": "Shared deterministic VBuffer, fixed scene time, native accumulation reset after warmup, native GPU Profiler",
        "comparison": "Visual and linear-HDR distance to stock RTXDI; not ground-truth error or exact paper reproduction",
    }
    save(output / "config.json", config)
    cases = []
    state = {"status": "running", "config": config, "cases": cases}
    save(output / "suite.json", state)
    try:
        for index, scene in enumerate(selected, 1):
            print(f"[{index}/{len(selected)}] {scene['name']}", flush=True)
            case = {"id": scene["id"], "name": scene["name"], "differences": scene.get("differences", [])}
            cases.append(case)
            path, reason = resolve_scene(scene, args.assets_root)
            if reason:
                case.update(status="skipped", reason=reason)
                print("  跳过: " + reason, flush=True)
                save(output / "suite.json", state)
                continue
            directory = output / scene["id"]
            directory.mkdir()
            save(directory / "config.json", {**config, "scene": scene, "resolved_scene": str(path)})
            case["status"] = "running"
            save(output / "suite.json", state)
            try:
                with (directory / "render.log").open("w", encoding="utf-8") as log:
                    result = subprocess.run(
                        [sys.executable, str(Path(__file__).with_name("suite_worker.py")), str(directory / "config.json")],
                        stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout, check=False, cwd=REPOSITORY,
                    )
                if result.returncode != 0:
                    status_path = directory / "status.json"
                    status = json.loads(status_path.read_text(encoding="utf-8")) if status_path.is_file() else {}
                    raise RuntimeError(status.get("error", "渲染进程退出码 " + str(result.returncode)) + "; 详见 render.log")
                if __package__:
                    from .report import render_case
                else:
                    from report import render_case
                render_case(directory, scene)
                case.update(status="completed", summary=json.loads((directory / "summary.json").read_text(encoding="utf-8")))
                print("  完成: " + str(directory / "comparison.png"), flush=True)
            except Exception as error:
                case.update(status="failed", reason=str(error))
                save(directory / "status.json", {"status": "failed", "error": str(error)})
                if (directory / "invalid_radiance.npz").is_file():
                    if __package__:
                        from .report import render_invalid_case
                    else:
                        from report import render_invalid_case
                    render_invalid_case(directory, scene)
                    case["diagnostic"] = "diagnostic_comparison.png"
                print("  失败: " + str(error), flush=True)
            save(output / "suite.json", state)
    except KeyboardInterrupt:
        state["status"] = "interrupted"
        save(output / "suite.json", state)
        raise
    if __package__:
        from .report import render_suite
    else:
        from report import render_suite
    render_suite(output, cases, config, include_html=args.html)
    completed = sum(case["status"] == "completed" for case in cases)
    failed = sum(case["status"] == "failed" for case in cases)
    skipped = sum(case["status"] == "skipped" for case in cases)
    state.update(status="failed" if failed or not completed else ("completed_with_skips" if skipped else "completed"),
                 counts={"selected": len(cases), "completed": completed, "failed": failed, "skipped": skipped})
    save(output / "suite.json", state)
    print(json.dumps({"status": state["status"], **state["counts"],
                      "data_figure": str(output / "data_summary.png"),
                      "comparison_figure": str(output / "comparison_summary.png")}, ensure_ascii=False))
    return 1 if failed or not completed or (skipped and args.require_complete) else 0


if __name__ == "__main__":
    raise SystemExit(main())
