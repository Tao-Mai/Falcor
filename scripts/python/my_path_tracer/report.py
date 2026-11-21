# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
"""从线性 HDR 结果生成对照图、差异曲线和可离线浏览的实验报告。"""

import csv
import html
import json
import os
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.colors import SymLogNorm
from matplotlib import font_manager
import numpy as np

_FONT_NAMES = {font.name for font in font_manager.fontManager.ttflist}
plt.rcParams["font.family"] = next((name for name in ("Microsoft YaHei", "SimHei", "Noto Sans CJK SC")
                                   if name in _FONT_NAMES), "DejaVu Sans")
plt.rcParams["axes.unicode_minus"] = False


def baseline_timing(summary):
    return summary.get("gpu_time_ms", {}).get(summary.get("baseline_pass", "RTXDIPass"), {})


def baseline_caption(config):
    return "Falcor PathTracer + RTXDI (DI only)" if config.get("baseline_renderer") == "path-tracer" else "Falcor RTXDIPass"


def display_exposure(baseline):
    luminance = np.maximum(baseline[..., :3], 0) @ np.array([0.2126, 0.7152, 0.0722])
    positive = luminance[luminance > 1e-8]
    level = float(np.percentile(positive, 75)) if positive.size else 0.18
    return float(np.clip(np.log2(0.18 / max(level, 1e-8)), -20, 20))


def display_rgb(linear, exposure_ev):
    """所有算法共用曝光、Reinhard 映射和标准 sRGB 编码；不修改 HDR 数据。"""
    exposed = np.maximum(linear[..., :3].astype(np.float64), 0) * (2.0 ** exposure_ev)
    mapped = exposed / (1 + exposed)
    return np.where(mapped <= 0.0031308, 12.92 * mapped, 1.055 * mapped ** (1 / 2.4) - 0.055)


def render_case(directory, scene):
    with np.load(directory / "linear_images.npz") as images:
        baseline = images["baseline"]
        candidate = images["candidate"]
    exposure = display_exposure(baseline)
    baseline_display = display_rgb(baseline, exposure)
    candidate_display = display_rgb(candidate, exposure)
    difference = np.mean(np.abs(candidate[..., :3].astype(np.float64) - baseline[..., :3]), axis=-1)
    positive_difference = difference[difference > 0]
    maximum = max(float(np.percentile(positive_difference, 99.5)), 1e-8) if positive_difference.size else 1e-8
    norm = SymLogNorm(linthresh=max(maximum * 0.01, 1e-10), vmin=0, vmax=maximum)
    plt.imsave(directory / "baseline.png", baseline_display)
    plt.imsave(directory / "my.png", candidate_display)
    plt.imsave(directory / "difference.png", plt.get_cmap("magma")(norm(difference)))
    summary = json.loads((directory / "summary.json").read_text(encoding="utf-8"))
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.1), layout="constrained")
    axes[0].imshow(baseline_display)
    axes[0].set_title("Baseline: " + summary.get("baseline_label", "native RTXDIPass"))
    axes[1].imshow(candidate_display)
    axes[1].set_title("MyPathTracer")
    heatmap = axes[2].imshow(difference, cmap="magma", norm=norm)
    axes[2].set_title("Mean absolute RGB difference (linear HDR)")
    fig.colorbar(heatmap, ax=axes[2], fraction=0.045, pad=0.03)
    for axis in axes:
        axis.set_axis_off()
    mse = summary["metrics"]["mse_to_baseline_rgb"]
    fig.suptitle(f"{scene['name']} | {summary['frames']} accumulated frames | shared EV {exposure:+.2f} | RGB MSE to baseline {mse:.4g}")
    for extension in ("png", "svg"):
        fig.savefig(directory / ("comparison." + extension), dpi=160)
    plt.close(fig)

    display = {"exposure_ev": exposure, "mapping": "shared exposure + Reinhard + sRGB",
               "difference_mapping": "linear-HDR mean absolute RGB, symmetric log color scale",
               "difference_display_max": maximum, "reference": "stock RTXDI, not ground truth"}
    (directory / "display.json").write_text(json.dumps(display, indent=2) + "\n", encoding="utf-8")
    rows = json.loads((directory / "metrics.json").read_text(encoding="utf-8"))
    capture = json.loads((directory / "profiler.json").read_text(encoding="utf-8"))
    fig, axes = plt.subplots(1, 2, figsize=(11, 3.5), layout="constrained")
    x = [row["accumulated_frames"] for row in rows]
    y = [row["mse_to_baseline_rgb"] for row in rows]
    axes[0].plot(x, y, marker=".", color="#2864b7")
    axes[0].set(xlabel="Accumulated frames", ylabel="RGB MSE to RTXDI", title="Distance to baseline (not ground truth error)")
    if min(y) > 0:
        axes[0].set_yscale("log")
    for name, label in ((summary.get("baseline_pass", "RTXDIPass"), "RTXDI"), ("MyPathTracer", "MyPathTracer")):
        lane = next(lane for key, lane in capture["events"].items() if key.endswith("/" + name + "/gpu_time"))
        axes[1].plot(x, lane["records"], label=label)
    axes[1].set(xlabel="Measurement frame", ylabel="GPU time (ms)", title="Native GPU scopes in shared graph")
    axes[1].legend()
    for axis in axes:
        axis.grid(alpha=0.25)
    fig.savefig(directory / "curves.png", dpi=160)
    fig.savefig(directory / "curves.svg")
    plt.close(fig)



def render_invalid_case(directory, scene):
    """保留失败帧的可视诊断；不生成有效误差指标或将失败改成成功。"""
    with np.load(directory / "invalid_radiance.npz") as images:
        baseline = images["baseline"]
        candidate = images["candidate"]
    masks = [~np.isfinite(image[..., :3]).all(-1) for image in (baseline, candidate)]
    cleaned = [np.where(np.isfinite(image), image, 0) for image in (baseline, candidate)]
    exposure = display_exposure(cleaned[0])
    shown = [display_rgb(image, exposure) for image in cleaned]
    for image, mask in zip(shown, masks):
        image[mask] = [1, 0, 1]
    plt.imsave(directory / "baseline_diagnostic.png", shown[0])
    plt.imsave(directory / "my_diagnostic.png", shown[1])
    delta = np.mean(np.abs(cleaned[0][..., :3].astype(np.float64) - cleaned[1][..., :3]), axis=-1)
    combined = masks[0] | masks[1]
    positive = delta[(delta > 0) & ~combined]
    maximum = max(float(np.percentile(positive, 99.5)), 1e-8) if positive.size else 1e-8
    norm = SymLogNorm(linthresh=max(maximum * 0.01, 1e-10), vmin=0, vmax=maximum)
    heatmap = plt.get_cmap("magma")(norm(delta))
    heatmap[combined] = [1, 0, 1, 1]
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.1), layout="constrained")
    titles = ("Native RTXDI (invalid pixels: " + str(int(masks[0].sum())) + ")",
              "MyPathTracer (invalid pixels: " + str(int(masks[1].sum())) + ")", "Difference preview / magenta = invalid")
    for axis, image, title in zip(axes, shown + [heatmap], titles):
        axis.imshow(image)
        axis.set_title(title)
        axis.set_axis_off()
    fig.suptitle(scene["name"] + " | DIAGNOSTIC: first invalid measurement frame; metrics unavailable", color="#ac1c32")
    fig.savefig(directory / "diagnostic_comparison.png", dpi=160)
    plt.close(fig)

def render_data_summary(output, cases, config):
    """独立 PNG/SVG 数据总图；失败项保留 N/A，不伪造指标。"""
    measured = [case for case in cases if case["status"] != "skipped"]
    if not measured:
        return
    fig = plt.figure(figsize=(13, 7.7), layout="constrained")
    grid = fig.add_gridspec(2, 2, height_ratios=(1.1, 1))
    timing_axis = fig.add_subplot(grid[0, 0])
    distance_axis = fig.add_subplot(grid[0, 1])
    table_axis = fig.add_subplot(grid[1, :])
    positions = np.arange(len(measured))
    names = [case["name"] for case in measured]
    for offset, name, label, color in ((-0.18, "RTXDIPass", "原生 RTXDI", "#2266a7"),
                                       (0.18, "MyPathTracer", "MyPathTracer", "#e59434")):
        lanes = [baseline_timing(case.get("summary", {})) if name == "RTXDIPass" else
                 case.get("summary", {}).get("gpu_time_ms", {}).get(name, {}) for case in measured]
        times = [lane.get("mean", np.nan) for lane in lanes]
        deviations = [lane.get("std_dev", 0) for lane in lanes]
        timing_axis.bar(positions + offset, times, 0.34, yerr=deviations, capsize=3, color=color, label=label)
    timing_axis.set(xticks=positions, xticklabels=names, ylabel="GPU scope 时间 / ms", title="GPU 时间：均值与逐帧标准差")
    timing_axis.tick_params(axis="x", labelrotation=12)
    timing_axis.set_xlim(-0.6, len(measured) - 0.4)
    timing_axis.legend()
    timing_axis.grid(axis="y", alpha=0.2)
    distances = [case.get("summary", {}).get("metrics", {}).get("mse_to_baseline_rgb", np.nan) for case in measured]
    distance_axis.bar(positions, distances, color="#697caf", width=0.55)
    if any(value > 0 for value in distances):
        distance_axis.set_yscale("log")
    distance_axis.set(xticks=positions, xticklabels=names, ylabel="线性 HDR RGB MSE（对数轴）", title="与 RTXDI 的距离：不是对真值的误差")
    distance_axis.tick_params(axis="x", labelrotation=12)
    distance_axis.set_xlim(-0.6, len(measured) - 0.4)
    distance_axis.grid(axis="y", alpha=0.2)
    rows = []
    for index, case in enumerate(measured):
        summary = case.get("summary", {})
        metrics = summary.get("metrics", {})
        timing = summary.get("gpu_time_ms", {})
        valid = case["status"] == "completed"
        if not valid:
            timing_axis.text(index, 0, "失败 / N/A", ha="center", va="bottom", color="#ae2334", fontsize=9)
            distance_axis.text(index, 0.03, "失败 / N/A", transform=distance_axis.get_xaxis_transform(),
                               ha="center", va="bottom", color="#ae2334", fontsize=9)
        rows.append([
            case["name"], str(summary.get("frames", "—")),
            f"{baseline_timing(summary)['mean']:.3f}" if valid and baseline_timing(summary) else "N/A",
            f"{timing['MyPathTracer']['mean']:.3f}" if valid and "MyPathTracer" in timing else "N/A",
            f"{metrics['mse_to_baseline_rgb']:.4g}" if valid else "N/A",
            f"{metrics.get('mae_to_baseline_rgb', np.nan):.4g}" if valid else "N/A",
            "完成" if valid else "失败：非有限值" if case.get("diagnostic") else "失败",
        ])
    table_axis.set_axis_off()
    table = table_axis.table(cellText=rows, colLabels=["场景", "累积帧", "RTXDI / ms", "My / ms", "MSE→RTXDI", "MAE→RTXDI", "状态"],
                             colWidths=[0.23, 0.09, 0.13, 0.12, 0.15, 0.15, 0.15], loc="center", cellLoc="center")
    table.auto_set_font_size(False)
    table.set_fontsize(10)
    table.scale(1, 1.9)
    for (row, column), cell in table.get_celld().items():
        cell.set_edgecolor("#d6dde7")
        if row == 0:
            cell.set_facecolor("#e8eef7")
        elif measured[row - 1]["status"] != "completed":
            cell.set_facecolor("#fff0f0")
        elif row % 2 == 0:
            cell.set_facecolor("#f7f9fc")
    resolution = config.get("resolution", ["?", "?"])
    fig.suptitle(f"DI 数据对比总图  |  {resolution[0]}×{resolution[1]}  |  预热 {config.get('warmup_frames', '?')} 帧 / 累积 {config.get('frames', '?')} 帧", fontsize=15)
    fig.supxlabel("Baseline: " + baseline_caption(config) + "\n计时为共享图原生 pass scope；距离指标不是对真值的误差。", fontsize=10)
    for extension in ("png", "svg"):
        fig.savefig(output / ("data_summary." + extension), dpi=170)
    plt.close(fig)


def render_comparison_summary(output, data_root, cases, config):
    visible = [case for case in cases if case["status"] == "completed" or case.get("diagnostic")]
    if not visible:
        return
    fig, axes = plt.subplots(len(visible), 4, squeeze=False, figsize=(17, 2.85 * len(visible) + 0.65),
                             gridspec_kw={"width_ratios": [1, 1, 1, 0.85]}, layout="constrained")
    for row, case in enumerate(visible):
        directory = data_root / case["id"]
        diagnostic = case["status"] != "completed"
        pair = ("baseline_diagnostic.png", "my_diagnostic.png") if diagnostic else ("baseline.png", "my.png")
        for column, filename in enumerate(pair):
            axes[row, column].imshow(plt.imread(directory / filename))
            axes[row, column].set_title(("原生 RTXDI" if column == 0 else "MyPathTracer") + " / " + case["name"], fontsize=10)
        if diagnostic:
            with np.load(directory / "invalid_radiance.npz") as images:
                a, b = images["baseline"], images["candidate"]
            mask = ~np.isfinite(a[..., :3]).all(-1) | ~np.isfinite(b[..., :3]).all(-1)
            delta = np.mean(np.abs(np.nan_to_num(a[..., :3]).astype(np.float64) - np.nan_to_num(b[..., :3])), axis=-1)
            scale = max(float(np.percentile(delta[~mask], 99.5)), 1e-8)
            heatmap = plt.get_cmap("magma")(SymLogNorm(linthresh=scale * 0.01, vmin=0, vmax=scale)(delta))
            heatmap[mask] = [1, 0, 1, 1]
            axes[row, 2].imshow(heatmap)
            text = "失败：RTXDI 支路非有限值\n\n首个异常测量帧的诊断图\n紫色 = 无效像素\n\nGPU / MSE / MAE：N/A"
        else:
            axes[row, 2].imshow(plt.imread(directory / "difference.png"))
            summary = case["summary"]
            timing, metrics = summary.get("gpu_time_ms", {}), summary["metrics"]
            text = (f"完成 / 累积 {summary['frames']} 帧\n\n"
                    f"RTXDI：{baseline_timing(summary).get('mean', np.nan):.3f} ms\n"
                    f"My：{timing.get('MyPathTracer', {}).get('mean', np.nan):.3f} ms\n\n"
                    f"MSE→RTXDI：{metrics['mse_to_baseline_rgb']:.4g}\n"
                    f"MAE→RTXDI：{metrics.get('mae_to_baseline_rgb', np.nan):.4g}")
        axes[row, 2].set_title("HDR 差异" + (" / 诊断" if diagnostic else " / 对数色标"), fontsize=10)
        axes[row, 3].text(0.02, 0.5, text, transform=axes[row, 3].transAxes, va="center", fontsize=11,
                         color="#ae2334" if diagnostic else "#26374b", linespacing=1.5)
        for axis in axes[row]:
            axis.set_axis_off()
    fig.suptitle("DI 场景：原生 RTXDI 与 MyPathTracer 的图像和数据对比", fontsize=16)
    fig.supxlabel("Baseline: " + baseline_caption(config) + "\n每个场景共用曝光；差异指标不是对真值的误差。GPU 数值为共享图中的 pass scope。", fontsize=10)
    fig.savefig(output / "comparison_summary.png", dpi=120)
    fig.savefig(output / "comparison_summary.svg")
    plt.close(fig)


def render_suite(output, cases, config, include_html=False, data_root=None):
    data_root = Path(data_root) if data_root is not None else output
    render_data_summary(output, cases, config)
    render_comparison_summary(output, data_root, cases, config)
    completed = [case for case in cases if case["status"] == "completed"]
    visible = [case for case in cases if case["status"] == "completed" or case.get("diagnostic")]
    if visible:
        fig, axes = plt.subplots(len(visible), 2, squeeze=False,
                                 figsize=(10, max(3, 2.8 * len(visible))), layout="constrained")
        for row, case in enumerate(visible):
            directory = data_root / case["id"]
            diagnostic = case["status"] != "completed"
            filenames = ("baseline_diagnostic.png", "my_diagnostic.png") if diagnostic else ("baseline.png", "my.png")
            for column, (filename, label) in enumerate(zip(filenames, ("Native RTXDI", "MyPathTracer"))):
                axes[row, column].imshow(plt.imread(directory / filename))
                axes[row, column].set_title(case["name"] + " / " + label + (" [DIAGNOSTIC]" if diagnostic else ""),
                                            color="#ac1c32" if diagnostic else "black")
                axes[row, column].set_axis_off()
        fig.savefig(output / "overview.png", dpi=150)
        plt.close(fig)
    rows = []
    for case in cases:
        summary = case.get("summary", {})
        timing = summary.get("gpu_time_ms", {})
        rows.append({
            "scene": case["id"], "status": case["status"], "reason": case.get("reason", ""),
            "frames": summary.get("frames", ""),
            "baseline_renderer": summary.get("baseline_renderer", "rtxdi-pass"),
            "mse_to_baseline_rgb": summary.get("metrics", {}).get("mse_to_baseline_rgb", ""),
            "mae_to_baseline_rgb": summary.get("metrics", {}).get("mae_to_baseline_rgb", ""),
            "my_gpu_ms": timing.get("MyPathTracer", {}).get("mean", ""),
            "rtxdi_gpu_ms": baseline_timing(summary).get("mean", ""),
        })
    if rows:
        with (output / "summary.csv").open("w", newline="", encoding="utf-8-sig") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
    failed = sum(case["status"] == "failed" for case in cases)
    skipped = sum(case["status"] == "skipped" for case in cases)
    if not include_html:
        return
    blocks = ["<!doctype html><html lang='zh-CN'><meta charset='utf-8'><meta name='viewport' content='width=device-width'>",
              "<title>MyPathTracer 与原生 RTXDI 对比</title><style>body{font:16px system-ui;max-width:1440px;margin:40px auto;padding:0 20px;background:#f5f6f8;color:#17212b}article{background:white;padding:22px;margin:24px 0;border-radius:12px}img{max-width:100%}a{color:#235bb5}small{color:#566}code{background:#eef1f6;padding:3px}li{margin:8px 0}</style>",
              "<h1>MyPathTracer 与原生 RTXDI 对比</h1>",
              f"<p>完成 {len(completed)} / {len(cases)}，跳过 {skipped}，失败 {failed}。套件：{html.escape(config['suite'])}。</p>",
              "<p>每个场景两张图使用同一曝光与显示变换。指标基于线性 HDR；RTXDI 是算法 baseline，并非真值。GPU 数值来自共享图中的原生 pass scope，不作为隔离的等时性能结论。</p>",
              "<p><a href='summary.csv'>汇总 CSV</a> · <a href='suite.json'>完整配置与状态</a></p>"]
    if visible:
        blocks.append("<p><a href='overview.png'>场景对照总图（异常配置单独标记）</a></p>")
    for case in cases:
        identifier = html.escape(Path(os.path.relpath(data_root / case["id"], output)).as_posix())
        blocks.append("<article><h2>" + html.escape(case["name"]) + "</h2>")
        if case["status"] == "completed":
            blocks.extend([
                f"<a href='{identifier}/comparison.png'><img src='{identifier}/comparison.png' alt='原生 RTXDI、MyPathTracer 和线性 HDR 差异图'></a>",
                f"<p><a href='{identifier}/comparison.svg'>矢量对照图</a> · <a href='{identifier}/curves.png'>差异与 GPU 时间曲线</a> · <a href='{identifier}/baseline.exr'>RTXDI EXR</a> · <a href='{identifier}/my.exr'>MyPathTracer EXR</a> · <a href='{identifier}/provenance.json'>场景、参数与环境</a></p>",
            ])
            for warning in case["summary"].get("warnings", []):
                blocks.append("<p><strong>说明：</strong>" + html.escape(warning) + "</p>")
        else:
            blocks.append("<p>" + html.escape(case["status"] + ": " + case.get("reason", "")) + "</p>")
            if case.get("diagnostic"):
                blocks.append(f"<p>以下为首个异常测量帧的诊断图，紫色标记无效像素，不计入有效指标。</p><img src='{identifier}/diagnostic_comparison.png' alt='失败帧诊断图'>")
        blocks.append("<ul>" + "".join("<li>" + html.escape(text) + "</li>" for text in case.get("differences", [])) + "</ul>")
        blocks.append("</article>")
    blocks.append("</html>")
    (output / "index.html").write_text("\n".join(blocks), encoding="utf-8")
