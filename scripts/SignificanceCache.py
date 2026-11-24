from falcor import RenderGraph


def render_graph_SignificanceCache():
    graph = RenderGraph("SignificanceCache")
    graph.create_pass("VBufferRT", "VBufferRT", {"samplePattern": "Stratified", "sampleCount": 16})
    graph.create_pass("SignificanceCache", "SignificanceCache")
    graph.create_pass("AccumulatePass", "AccumulatePass", {"enabled": True, "precisionMode": "SingleCompensated"})
    graph.create_pass("ToneMapper", "ToneMapper", {"autoExposure": False, "exposureCompensation": 0.0})
    graph.add_edge("VBufferRT.vbuffer", "SignificanceCache.vbuffer")
    graph.add_edge("VBufferRT.viewW", "SignificanceCache.viewW")
    graph.add_edge("SignificanceCache.color", "AccumulatePass.input")
    graph.add_edge("AccumulatePass.output", "ToneMapper.src")
    graph.mark_output("ToneMapper.dst")
    return graph


if "m" in globals():
    m.addGraph(render_graph_SignificanceCache())
