# Copyright (c) 2026, Falcor project contributors.
# SPDX-License-Identifier: BSD-3-Clause
"""RenderGraph construction shared by experiments and Python regression tests."""


def create_comparison_graph(testbed, properties=None, bind_view=True, measurements_path=None):
    """Compare independent passes using identical primary hits and PRNG dimensions.

    Outputs are raw RGBA32Float radiance. The graph deliberately has no display
    transform or accumulation, so neither can conceal an estimator difference.
    """
    graph = testbed.create_render_graph("MyPathTracerComparison")
    graph.create_pass("VBufferRT", "VBufferRT", {"samplePattern": "Center", "useDOF": False})
    for name in ("MinimalPathTracer", "MyPathTracer"):
        graph.create_pass(name, name, dict(properties or {}))
        graph.add_edge("VBufferRT.vbuffer", name + ".vbuffer")
        if bind_view:
            graph.add_edge("VBufferRT.viewW", name + ".viewW")
        graph.mark_output(name + ".color")

    if measurements_path is not None:
        graph.create_pass(
            "ErrorMeasurePass",
            "ErrorMeasurePass",
            {
                "MeasurementsFilePath": str(measurements_path),
                "UseLoadedReference": False,
                "IgnoreBackground": False,
                "ComputeSquaredDifference": True,
                "ComputeAverage": True,
                "ReportRunningError": False,
                "SelectedOutputId": "Difference",
            },
        )
        graph.add_edge("MyPathTracer.color", "ErrorMeasurePass.Source")
        graph.add_edge("MinimalPathTracer.color", "ErrorMeasurePass.Reference")
        graph.mark_output("ErrorMeasurePass.Output")
    return graph


def create_rtxdi_comparison_graph(testbed, properties=None, rtxdi_options=None, measurements_path=None, baseline_renderer="path-tracer"):
    """Compare against native PathTracer with RTXDI and direct lighting only.

    Both algorithms share primary hits and a fixed pinhole camera. Independent
    native accumulators average the measurement interval after warmup. RTXDI's
    native reservoir history remains active and is never patched by this graph.
    """
    graph = testbed.create_render_graph("MyPathTracerVsRTXDI")
    graph.create_pass("VBufferRT", "VBufferRT", {"samplePattern": "Center", "useDOF": False})
    graph.create_pass("MyPathTracer", "MyPathTracer", dict(properties or {}))
    if baseline_renderer == "path-tracer":
        graph.create_pass("RTXDIBaseline", "PathTracer", {
            "useRTXDI": True, "RTXDIOptions": dict(rtxdi_options or {}), "useReSTIRPT": False,
            "maxSurfaceBounces": 0, "maxDiffuseBounces": 0, "maxSpecularBounces": 0, "maxTransmissionBounces": 0,
            "samplesPerPixel": 1, "colorFormat": "RGBA32F",
        })
        graph.add_edge("VBufferRT.viewW", "RTXDIBaseline.viewW")
    elif baseline_renderer == "rtxdi-pass":
        graph.create_pass("RTXDIBaseline", "RTXDIPass", {"options": dict(rtxdi_options or {})})
    else:
        raise ValueError("Unknown native RTXDI integration: " + baseline_renderer)
    graph.add_edge("VBufferRT.vbuffer", "MyPathTracer.vbuffer")
    graph.add_edge("VBufferRT.viewW", "MyPathTracer.viewW")
    graph.add_edge("VBufferRT.vbuffer", "RTXDIBaseline.vbuffer")
    graph.add_edge("VBufferRT.mvec", "RTXDIBaseline.mvec")
    for name, source in (("BaselineAccumulate", "RTXDIBaseline"), ("MyAccumulate", "MyPathTracer")):
        graph.create_pass(name, "AccumulatePass", {"enabled": True, "precisionMode": "Single"})
        graph.add_edge(source + ".color", name + ".input")
        graph.mark_output(name + ".output")
    if measurements_path is not None:
        graph.create_pass("ErrorMeasurePass", "ErrorMeasurePass", {
            "MeasurementsFilePath": str(measurements_path),
            "UseLoadedReference": False, "IgnoreBackground": False,
            "ComputeSquaredDifference": True, "ComputeAverage": True,
            "ReportRunningError": False, "SelectedOutputId": "Difference",
        })
        graph.add_edge("MyAccumulate.output", "ErrorMeasurePass.Source")
        graph.add_edge("BaselineAccumulate.output", "ErrorMeasurePass.Reference")
        graph.mark_output("ErrorMeasurePass.Output")
    # Keep the public accumulator output indices stable for native EXR capture.
    # Raw outputs are retained as well so validation can identify the stage that
    # first produced a nonfinite value, including during warmup.
    graph.mark_output("RTXDIBaseline.color")
    graph.mark_output("MyPathTracer.color")
    return graph
