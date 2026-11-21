# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
"""使用公开 Zero Day 资产回归原生 RTXDI 基线；未安装资产时明确跳过。"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import numpy as np

REPOSITORY = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "scripts/python"))
from my_path_tracer.scenes import default_assets_root, load_catalog, resolve_scene, select_scenes


class TestNativeRTXDIBaseline(unittest.TestCase):
    def test_zero_day_native_rtxdi_direct_lighting_is_finite(self):
        catalog = load_catalog()
        scene = select_scenes(catalog, "paper", ["zero_day"])[0]
        source, reason = resolve_scene(scene, default_assets_root())
        if source is None:
            self.skipTest(reason)
        with tempfile.TemporaryDirectory(prefix="falcor-rtxdi-") as temporary:
            directory = Path(temporary)
            config = {
                "scene": scene, "resolved_scene": str(source), "resolution": [640, 360], "frames": 64,
                "warmup_frames": 32, "device_type": "d3d12" if sys.platform == "win32" else "vulkan", "gpu": 0,
                "baseline_renderer": "path-tracer", "baseline": catalog["baseline"], "rtxdi_options": {},
                "my_properties": {"maxBounces": 0, "computeDirect": True, "useImportanceSampling": True},
            }
            (directory / "config.json").write_text(json.dumps(config), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(REPOSITORY / "scripts/python/my_path_tracer/suite_worker.py"), str(directory / "config.json")],
                cwd=REPOSITORY, capture_output=True, text=True, errors="replace", timeout=900, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout[-2000:] + result.stderr[-4000:])
            summary = json.loads((directory / "summary.json").read_text(encoding="utf-8"))
            self.assertEqual(summary["radiance_validation"]["checked_frames"], 98)
            self.assertEqual(summary["radiance_validation"]["nonfinite_components"], 0)
            provenance = json.loads((directory / "provenance.json").read_text(encoding="utf-8"))
            effective = provenance["effective_properties"]["RTXDIBaseline"]
            self.assertTrue(effective["useRTXDI"])
            self.assertFalse(effective["useReSTIRPT"])
            for key in ("maxSurfaceBounces", "maxDiffuseBounces", "maxSpecularBounces", "maxTransmissionBounces"):
                self.assertEqual(effective[key], 0, key)
            with np.load(directory / "linear_images.npz") as images:
                self.assertTrue(np.isfinite(images["baseline"]).all())
                self.assertTrue(np.isfinite(images["candidate"]).all())


if __name__ == "__main__":
    unittest.main()
