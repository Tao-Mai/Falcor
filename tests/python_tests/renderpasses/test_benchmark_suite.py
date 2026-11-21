# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
"""验证实验场景选择和对比图的公共曝光，防止将显示调整当作算法差异。"""

from pathlib import Path
import json
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

import numpy as np

REPOSITORY = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "scripts/python"))
from my_path_tracer.scenes import load_catalog, prepare_mitsuba_scene, resolve_scene, select_scenes
from my_path_tracer.validation import RADIANCE_OUTPUTS, read_checked_radiance


class TestRadianceValidation(unittest.TestCase):
    def test_raw_warmup_failure_is_recorded_before_accumulated_metrics(self):
        images = {name: np.ones((3, 4, 4), dtype=np.float32) for name in RADIANCE_OUTPUTS.values()}
        images["RTXDIBaseline.color"][2, 1, :3] = np.nan

        class Texture:
            def __init__(self, value):
                self.value = value

            def to_numpy(self):
                return self.value

        class Graph:
            def get_output(self, name):
                return Texture(images[name])

        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            with self.assertRaisesRegex(RuntimeError, "warmup.*frame 2"):
                read_checked_radiance(Graph(), output, "warmup", 2)
            record = json.loads((output / "invalid_radiance.json").read_text(encoding="utf-8"))
            self.assertEqual(record["phase"], "warmup")
            self.assertEqual(record["inspected_outputs"]["raw_baseline"], "RTXDIBaseline.color")
            self.assertEqual(record["first_invalid_pixels_xy"]["raw_baseline"], [[1, 2]])
            self.assertEqual(record["nonfinite_components"]["baseline"], 0)
            with np.load(output / "invalid_radiance.npz") as saved:
                self.assertTrue(np.isnan(saved["raw_baseline"][2, 1, :3]).all())
                self.assertTrue(np.isfinite(saved["baseline"]).all())
            self.assertFalse((output / "metrics.json").exists())


class TestBenchmarkCatalog(unittest.TestCase):
    def test_di_suites_do_not_include_gi_research_scenes(self):
        catalog = load_catalog()
        gi_scenes = {"veach_ajar", "veach_bidir", "sun_temple"}
        for name in catalog["suites"]:
            self.assertFalse(gi_scenes.intersection(scene["id"] for scene in select_scenes(catalog, name)))
        available = {scene["id"] for scene in select_scenes(catalog, "paper") if not scene.get("unavailable")}
        self.assertEqual(available, {"bistro_exterior", "bistro_interior", "emerald_square", "zero_day"})
        with self.assertRaises(ValueError):
            select_scenes(catalog, "all", ["veach_ajar"])

    def test_main_scene_coverage_and_explicit_missing_assets(self):
        catalog = load_catalog()
        selected = select_scenes(catalog, "restir2020")
        self.assertEqual({scene["id"] for scene in selected}, {
            "bistro_exterior", "emerald_square", "zero_day", "burger_restaurant",
            "subway", "soda_hall", "amusement_park",
        })
        missing = [scene for scene in selected if scene.get("unavailable")]
        self.assertEqual(len(missing), 4)
        for scene in missing:
            path, reason = resolve_scene(scene, Path(tempfile.gettempdir()))
            self.assertIsNone(path)
            self.assertTrue(reason)
        with self.assertRaises(ValueError):
            select_scenes(catalog, "paper", ["unknown_scene"])

    def test_assets_root_and_missing_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "scene.xml"
            path.write_text("scene", encoding="utf-8")
            scene = {"root": "assets", "path": "scene.xml"}
            resolved, reason = resolve_scene(scene, root)
            self.assertEqual(resolved, path.resolve())
            self.assertIsNone(reason)
            scene["path"] = "../scene.xml"
            with self.assertRaises(ValueError):
                resolve_scene(scene, root)
            scene["path"] = "missing.xml"
            resolved, reason = resolve_scene(scene, root)
            self.assertIsNone(resolved)
            self.assertIn("missing.xml", reason)

    def test_mitsuba_compatibility_preserves_source_and_world_translation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "mesh.obj").write_text("fixture", encoding="utf-8")
            source = root / "source.xml"
            contents = '<scene><default name="depth" value="4"/><integer name="max_depth" value="$depth"/>'
            contents += '<bsdf type="twosided" id="wall"><bsdf type="diffuse"/></bsdf>'
            contents += '<string name="filename" value="mesh.obj"/><matrix value="1 0 0 2 0 1 0 3 0 0 1 4 0 0 0 1"/></scene>'
            source.write_text(contents, encoding="utf-8")
            prepared = prepare_mitsuba_scene(source, root)
            parsed = ET.parse(prepared).getroot()
            self.assertEqual(source.read_text(encoding="utf-8"), contents)
            self.assertEqual(parsed.findall("default"), [])
            self.assertEqual(parsed.find("integer").get("value"), "4")
            self.assertEqual(parsed.find("bsdf").get("type"), "diffuse")
            self.assertEqual(parsed.find("bsdf").get("id"), "wall")
            self.assertEqual(Path(parsed.find("string").get("value")), root / "mesh.obj")
            tokens = [float(value) for value in parsed.find("matrix").get("value").split()]
            # Falcor's importer transposes its matrix input. The resulting world
            # transform must retain the asset's translation (2, 3, 4).
            native_matrix = np.array(tokens).reshape(4, 4).T
            np.testing.assert_array_equal(native_matrix[:3, 3], [2, 3, 4])
            np.testing.assert_array_equal(native_matrix[3], [0, 0, 0, 1])


class TestBenchmarkDisplay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            from my_path_tracer.report import display_exposure, display_rgb
        except ImportError as error:
            raise unittest.SkipTest("安装 benchmark requirements.txt 后可验证图像报告: " + str(error))
        cls.exposure = staticmethod(display_exposure)
        cls.display = staticmethod(display_rgb)

    def test_common_exposure_preserves_brightness_difference(self):
        baseline = np.ones((2, 3, 4), dtype=np.float32)
        candidate = baseline.copy()
        candidate[..., :3] *= 0.25
        original = candidate.copy()
        exposure = self.exposure(baseline)
        shown_baseline = self.display(baseline, exposure)
        shown_candidate = self.display(candidate, exposure)
        self.assertTrue(np.all(shown_candidate < shown_baseline))
        np.testing.assert_array_equal(candidate, original)
        self.assertEqual(shown_candidate.shape, (2, 3, 3))
        self.assertTrue(np.all((shown_candidate >= 0) & (shown_candidate <= 1)))

    def test_known_reinhard_and_srgb_values(self):
        image = np.array([[[0, 1, 3, 1]]], dtype=np.float64)
        expected = np.array([[[0.0, 0.7353569830524495, 0.8808250210902997]]])
        np.testing.assert_allclose(self.display(image, 0), expected, rtol=0, atol=1e-12)

    def test_rendered_report_and_nonfinite_diagnostic_stay_separate(self):
        from my_path_tracer.report import render_case, render_invalid_case, render_suite
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            valid = root / "valid"
            invalid = root / "invalid"
            valid.mkdir()
            invalid.mkdir()
            baseline = np.ones((4, 6, 4), dtype=np.float32)
            candidate = baseline.copy()
            candidate[..., :3] *= 0.5
            np.savez(valid / "linear_images.npz", baseline=baseline, candidate=candidate)
            row = {"accumulated_frames": 1, "mse_to_baseline_rgb": 0.25}
            (valid / "summary.json").write_text(json.dumps({"frames": 1, "metrics": row}))
            (valid / "metrics.json").write_text(json.dumps([row]))
            (valid / "profiler.json").write_text(json.dumps({"events": {
                "/RTXDIPass/gpu_time": {"records": [1.0]},
                "/MyPathTracer/gpu_time": {"records": [2.0]},
            }}))
            render_case(valid, {"name": "Fixture"})
            for name in ("comparison.png", "comparison.svg", "curves.png", "display.json"):
                self.assertTrue((valid / name).is_file(), name)
            baseline[0, 0, :3] = np.nan
            np.savez(invalid / "invalid_radiance.npz", baseline=baseline, candidate=candidate)
            render_invalid_case(invalid, {"name": "Invalid fixture"})
            self.assertTrue((invalid / "diagnostic_comparison.png").is_file())
            self.assertFalse((invalid / "metrics.json").exists())
            self.assertFalse((invalid / "summary.json").exists())
            render_suite(root, [
                {"id": "valid", "name": "Valid", "status": "completed", "summary": {"frames": 1, "metrics": row}},
                {"id": "invalid", "name": "Invalid", "status": "failed", "diagnostic": "diagnostic_comparison.png"},
            ], {"suite": "fixture"})
            self.assertTrue((root / "overview.png").is_file())
            self.assertTrue((root / "data_summary.png").is_file())
            self.assertTrue((root / "comparison_summary.png").is_file())
            self.assertFalse((root / "index.html").exists())
            render_suite(root, [
                {"id": "valid", "name": "Valid", "status": "completed", "summary": {"frames": 1, "metrics": row}},
                {"id": "invalid", "name": "Invalid", "status": "failed", "diagnostic": "diagnostic_comparison.png"},
            ], {"suite": "fixture"}, include_html=True)
            self.assertIn("invalid/diagnostic_comparison.png", (root / "index.html").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
