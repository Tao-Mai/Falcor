"""验证默认图注册、冷缓存预热、参数校验以及场景变化后的缓存失效。"""

import json
import traceback
from pathlib import Path

import numpy as np
from falcor import exit as exit_mogwai, float3


ROOT = Path(__file__).resolve().parents[2]
RESULTS = {}


def run(renderer):
    renderer.script(str(ROOT / "scripts/my_graph/path_reuse.py"))
    names = ("PathReuse", "MyPathTracer", "PG5DTree", "PhotonMapPathTracer", "SignificanceCache")
    for name in names:
        assert renderer.getGraph(name) is not None, name
    RESULTS["registered_graphs"] = names
    graph = renderer.getGraph("SignificanceCache")
    graph.update_pass("VBufferRT", {"samplePattern": "Center", "adjustShadingNormals": False})
    graph.mark_output("SignificanceCache.color")
    graph.mark_output("SignificanceCache.guide")
    renderer.setActiveGraph(graph)
    renderer.profiler.enabled = True
    renderer.unloadScene()

    def frame():
        renderer.renderFrame()
        color = graph.get_output("SignificanceCache.color").to_numpy()
        guide = graph.get_output("SignificanceCache.guide").to_numpy()
        assert np.isfinite(color).all() and np.isfinite(guide).all()
        reset = any("ResetCache" in event for event in renderer.profiler.events)
        active = float(np.mean(guide[..., 2] > 0))
        return color.copy(), active, reset

    color, active, _ = frame()
    assert np.all(color[..., :3] == 0) and active == 0, (float(np.max(color[..., :3])), active)
    RESULTS["no_scene_clear"] = True

    renderer.loadScene(str(ROOT / "resource/CornellBox/CornellBox.pyscene"))
    for index in range(1, 9):
        _, active, reset = frame()
        assert reset == (index == 1), (index, reset)
        assert (active == 0) if index <= 4 else (active > 0)
    RESULTS["warmup_and_snapshot"] = True

    def change(name, operation):
        operation()
        _, active, reset = frame()
        assert reset and active == 0, (name, active, reset)
        for _ in range(4):
            _, active, reset = frame()
            assert not reset
        RESULTS[name] = True
        print(name + " 验证通过", flush=True)

    tracer = graph.get_pass("SignificanceCache")
    change("seed_reset", lambda: tracer.set_properties({"seed": 12345}))
    change("grid_resize", lambda: tracer.set_properties({"gridResolution": 8}))
    change("neighbor_search", lambda: tracer.set_properties({"searchNeighborCells": True}))
    change("query_lobe_count", lambda: tracer.set_properties({"queryLobes": 1}))
    lamp = renderer.scene.getMaterial("Emitter_Light_temp")
    change("emissive_intensity", lambda: setattr(lamp, "emissiveFactor", 80))
    change("render_settings", lambda: setattr(renderer.scene.renderSettings, "useEmissiveLights", False))
    change("render_settings_restore", lambda: setattr(renderer.scene.renderSettings, "useEmissiveLights", True))

    camera = renderer.scene.camera
    camera.position = camera.position + float3(1, 0, 0)
    _, active, reset = frame()
    assert not reset and active > 0
    RESULTS["camera_preserves_cache"] = True

    for properties in ({"gridResolution": 0}, {"gridResolution": 33}, {"guideMix": 1.0},
                       {"guideMix": float("nan")}, {"maxExponent": float("inf")},
                       {"updateInterval": 0}, {"maxBounces": 17}, {"guidedBounces": 17},
                       {"queryLobes": 0}, {"queryLobes": 9}):
        previous = tracer.properties
        try:
            tracer.set_properties(properties)
        except Exception:
            assert tracer.properties == previous
        else:
            raise AssertionError(f"未拒绝非法配置：{properties}")
    RESULTS["invalid_properties_rejected"] = True

    tracer.set_properties({"maxBounces": 0, "enableGuiding": True})
    first = frame()[0]
    tracer.set_properties({"enableGuiding": False})
    second = frame()[0]
    assert np.array_equal(first, second)
    RESULTS["zero_bounce_bypass"] = True

    tracer.set_properties({"maxBounces": 16, "enableGuiding": True, "queryLobes": 8,
                           "warmupFrames": 0, "updateInterval": 1})
    frame()
    assert frame()[1] > 0
    RESULTS["maximum_capacity_and_zero_warmup"] = True

    for name in ("MyPathTracer", "SignificanceCache"):
        renderer.setActiveGraph(renderer.getGraph(name))
        renderer.renderFrame()
    RESULTS["graph_switch"] = True
    output = ROOT / "build/significance-cache-experiment/runtime-validation.json"
    output.write_text(json.dumps(RESULTS, indent=2), encoding="utf-8")
    print(f"运行验证通过：{output}", flush=True)


if "m" in globals():
    try:
        run(m)
    except Exception:
        traceback.print_exc()
        exit_mogwai(1)
    else:
        exit_mogwai()
