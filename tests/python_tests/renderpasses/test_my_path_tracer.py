# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
"""Behavioral regression against the unmodified MinimalPathTracer plugin."""

from pathlib import Path
import sys
import unittest

import falcor
import numpy as np

REPOSITORY = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "scripts/python"))
from my_path_tracer.graphs import create_comparison_graph


class TestMyPathTracer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.testbed = falcor.Testbed(width=64, height=48, create_window=False)
        cls.testbed.show_ui = False
        cls.testbed.clock.time = 0
        cls.testbed.clock.pause()
        cls.scene = REPOSITORY / "media/test_scenes/cornell_box.pyscene"
        if not cls.scene.is_file():
            raise unittest.SkipTest("Falcor test media is required: " + str(cls.scene))

    def compare_frames(self, count=4):
        for frame in range(count):
            self.testbed.frame()
            original = self.graph.get_output("MinimalPathTracer.color").to_numpy().copy()
            candidate = self.graph.get_output("MyPathTracer.color").to_numpy().copy()
            self.assertTrue(np.isfinite(candidate).all())
            np.testing.assert_array_equal(candidate, original, err_msg="Raw RGBA differs at frame " + str(frame))

    def load_graph(self, properties=None, bind_view=True):
        self.graph = create_comparison_graph(self.testbed, properties, bind_view)
        self.testbed.render_graph = self.graph
        self.testbed.load_scene(str(self.scene))
        self.testbed.scene.animated = False

    def test_default_parameters_and_frames(self):
        self.load_graph()
        self.assertEqual(self.graph.get_pass("MyPathTracer").properties, self.graph.get_pass("MinimalPathTracer").properties)
        self.compare_frames(6)

    def test_parameter_updates(self):
        self.load_graph()
        cases = [
            {"maxBounces": 0, "computeDirect": True, "useImportanceSampling": True},
            {"maxBounces": 2, "computeDirect": False, "useImportanceSampling": True},
            {"maxBounces": 3, "computeDirect": True, "useImportanceSampling": False},
        ]
        for properties in cases:
            with self.subTest(properties=properties):
                for name in ("MinimalPathTracer", "MyPathTracer"):
                    self.graph.update_pass(name, properties)
                self.compare_frames()
                self.assertEqual(self.graph.get_pass("MyPathTracer").properties, properties)

    def test_optional_view_buffer(self):
        self.load_graph(bind_view=False)
        self.compare_frames()

    def test_resize_camera_and_scene_reload(self):
        self.load_graph()
        self.compare_frames(2)
        self.testbed.resize_frame_buffer(80, 60)
        self.testbed.scene.camera.position = falcor.float3(0.05, 0.28, 1.2)
        self.compare_frames()
        self.testbed.load_scene(str(self.scene))
        self.testbed.scene.animated = False
        self.compare_frames()
        self.testbed.resize_frame_buffer(64, 48)


if __name__ == "__main__":
    unittest.main()
