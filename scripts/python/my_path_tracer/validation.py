# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
"""检查原始辐射值与累积输出，记录第一个异常阶段，不替换异常数值。"""

import json

import numpy as np


RADIANCE_OUTPUTS = {
    "raw_baseline": "RTXDIBaseline.color",
    "raw_candidate": "MyPathTracer.color",
    "baseline": "BaselineAccumulate.output",
    "candidate": "MyAccumulate.output",
}


def read_checked_radiance(graph, output, phase, renderer_frame, measurement_frame=None):
    images = {key: graph.get_output(name).to_numpy().copy() for key, name in RADIANCE_OUTPUTS.items()}
    invalid = {key: int(np.count_nonzero(~np.isfinite(value))) for key, value in images.items()}
    if any(invalid.values()):
        np.savez_compressed(output / "invalid_radiance.npz", **images)
        details = {
            "phase": phase, "renderer_frame": renderer_frame, "frame": measurement_frame,
            "nonfinite_components": invalid, "inspected_outputs": RADIANCE_OUTPUTS,
            "first_invalid_pixels_xy": {
                key: np.argwhere(~np.isfinite(value).all(axis=-1))[:32, ::-1].tolist()
                for key, value in images.items()
            },
        }
        (output / "invalid_radiance.json").write_text(
            json.dumps(details, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8",
        )
        raise RuntimeError(f"{phase} 的 renderer frame {renderer_frame} 出现非有限辐射值: {invalid}")
    return images
