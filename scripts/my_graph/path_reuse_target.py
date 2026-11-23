from falcor import RenderGraph


def render_graph_PathReuseTarget(samples_per_pixel=16, max_surface_bounces=6, sample_pattern="Stratified"):
    graph = RenderGraph("PathReuseTarget")
    graph.create_pass(
        "VBufferRT", "VBufferRT", {"samplePattern": sample_pattern, "sampleCount": 16, "adjustShadingNormals": False}
    )
    graph.create_pass(
        "PathTracer",
        "PathTracer",
        {
            "samplesPerPixel": samples_per_pixel,
            "maxSurfaceBounces": max_surface_bounces,
            "useNEE": True,
            "useMIS": True,
            "misHeuristic": "PowerTwo",
            "emissiveSampler": "Uniform",
            "useRussianRoulette": False,
            "tracePassMode": "MinimalPayload",
            "colorFormat": "RGBA32F",
        },
    )
    graph.create_pass("AccumulatePass", "AccumulatePass", {"enabled": True, "precisionMode": "SingleCompensated"})
    graph.create_pass("ToneMapper", "ToneMapper", {"autoExposure": False, "exposureCompensation": 0.0})
    graph.add_edge("VBufferRT.vbuffer", "PathTracer.vbuffer")
    graph.add_edge("VBufferRT.viewW", "PathTracer.viewW")
    graph.add_edge("PathTracer.color", "AccumulatePass.input")
    graph.add_edge("AccumulatePass.output", "ToneMapper.src")
    graph.mark_output("ToneMapper.dst")
    return graph


if "m" in globals():
    PathReuseTarget = render_graph_PathReuseTarget()
    m.addGraph(PathReuseTarget)
