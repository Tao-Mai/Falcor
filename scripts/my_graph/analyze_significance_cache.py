"""从实验 JSON 和线性图像生成统一尺度的科学图表与中文结果摘要。"""

import argparse
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib import font_manager
import numpy as np


ROOT = Path(__file__).resolve().parents[2]
LABELS = {"BSDF": "BSDF 基线", "SC_primary": "SC 首跳", "SC_all": "SC 多跳"}
COLORS = {"BSDF": "#677487", "SC_primary": "#2679bc", "SC_all": "#d65b36"}
SCENES = {"CornellBox": "Cornell Box", "IndirectRoom": "遮挡光源房间", "FocusedRoom": "局部间接光房间"}


def display_rgb(image):
    positive = np.maximum(image, 0)
    return (positive / (1 + positive)) ** (1 / 2.2)


def analyze(result_path, output_dir):
    result = json.loads(result_path.read_text(encoding="utf-8"))
    output_dir.mkdir(parents=True, exist_ok=True)
    font_path = Path("C:/Windows/Fonts/msyh.ttc")
    if font_path.exists():
        font_manager.fontManager.addfont(str(font_path))
        plt.rcParams["font.family"] = font_manager.FontProperties(fname=str(font_path)).get_name()
    plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False, "axes.unicode_minus": False})
    checkpoints = result["checkpoints_spp"]
    fig, axes = plt.subplots(len(result["scenes"]), 2, figsize=(10.8, 3.8 * len(result["scenes"])), squeeze=False)
    summary = []
    for row, (name, scene) in enumerate(result["scenes"].items()):
        for mode, data in scene["modes"].items():
            errors = np.asarray(data["mean_mse"])
            uncertainty = 1.96 * np.asarray(data["mse_standard_error"])
            for col, x in enumerate((checkpoints, data["median_cumulative_gpu_ms"])):
                axes[row, col].loglog(x, errors, "o-", label=LABELS[mode], color=COLORS[mode], markersize=4)
                axes[row, col].fill_between(x, np.maximum(errors - uncertainty, errors * 0.01), errors + uncertainty,
                                            color=COLORS[mode], alpha=0.1)
                axes[row, col].grid(True, which="both", alpha=0.18)
                axes[row, col].set_ylabel("线性 RGB MSE")
                axes[row, col].set_title(SCENES.get(name, name))
                axes[row, col].set_xlabel("每像素样本数（spp）" if col == 0 else "累计 GPU 时间（ms，包含训练）")
        for ax in axes[row]:
            ax.legend()
            ax.axhline(scene["reference_mse_estimate"], linestyle=":", color="#888888", linewidth=0.8)

        baseline = scene["modes"]["BSDF"]
        summary += [f"### {SCENES.get(name, name)}", "",
                    f"参考图剩余噪声 MSE 估计：`{scene['reference_mse_estimate']:.6g}`。", "",
                    f"| 模式 | 64 spp MSE | {checkpoints[-1]} spp MSE | GPU ms/帧 | 同 spp MSE 降幅 |",
                    "| --- | ---: | ---: | ---: | ---: |"]
        for mode, data in scene["modes"].items():
            final = data["mean_mse"][-1]
            index = checkpoints.index(64) if 64 in checkpoints else -1
            reduction = 1 - final / baseline["mean_mse"][-1]
            summary.append(f"| {LABELS[mode]} | {data['mean_mse'][index]:.6g} | {final:.6g} | "
                           f"{data['median_cumulative_gpu_ms'][-1] / checkpoints[-1]:.3f} | {reduction:.1%} |")
        summary += ["", "| 相同 GPU 预算（ms） | BSDF MSE | SC 首跳 MSE | SC 多跳 MSE |", "| ---: | ---: | ---: | ---: |"]
        for index, budget in enumerate(scene["equal_time_budgets_ms"]):
            values = [scene["modes"][mode]["equal_time_mse"][index] for mode in LABELS]
            summary.append(f"| {budget:.2f} | " + " | ".join(f"{value:.6g}" for value in values) + " |")
        summary.append("")

        archive = result_path.parent / f"{name}.npz"
        if not archive.exists():
            continue
        images = np.load(archive)
        reference = images["reference"]
        shown_spp = 64 if f"BSDF_64spp" in images else checkpoints[-1]
        panel, panes = plt.subplots(2, 4, figsize=(12.8, 5.6), gridspec_kw={"height_ratios": [1, 1]})
        panes[0, 0].imshow(display_rgb(reference))
        panes[0, 0].set_title(f"参考 {scene.get('reference_spp', result['reference_spp'])} spp")
        panes[1, 0].axis("off")
        scale = max(float(np.percentile(np.sqrt(np.mean((images[f"BSDF_{shown_spp}spp"] - reference) ** 2, axis=2)), 97)), 1e-8)
        for col, mode in enumerate(LABELS, start=1):
            image = images[f"{mode}_{shown_spp}spp"]
            panes[0, col].imshow(display_rgb(image))
            panes[0, col].set_title(f"{LABELS[mode]} · {shown_spp} spp")
            error = np.sqrt(np.mean((image - reference) ** 2, axis=2))
            im = panes[1, col].imshow(error, cmap="magma", vmin=0, vmax=scale)
            panes[1, col].set_title("RGB RMS 误差（相同色标）")
        for ax in panes.flat:
            ax.set_xticks([])
            ax.set_yticks([])
        panel.suptitle(SCENES.get(name, name) + " · 统一 Reinhard + gamma 显示，误差使用线性 RGB", fontsize=12)
        panel.tight_layout(rect=(0, 0.03, 1, 0.95))
        panel.colorbar(im, ax=panes[1, 1:].tolist(), fraction=0.018, pad=0.02)
        panel.savefig(output_dir / f"{name}.png", dpi=160, bbox_inches="tight")
        plt.close(panel)

    fig.tight_layout()
    fig.savefig(output_dir / "convergence.png", dpi=170, bbox_inches="tight")
    plt.close(fig)
    (output_dir / "summary.md").write_text("\n".join(summary), encoding="utf-8")
    (output_dir / "results.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"图表和摘要已写入 {output_dir}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", type=Path, default=ROOT / "build/significance-cache-experiment/results.json")
    parser.add_argument("--output", type=Path, default=ROOT / "Source/MyRenderPasses/SignificanceCache/Experiments")
    args = parser.parse_args()
    analyze(args.result, args.output)
