from falcor import RenderGraph


def render_graph_PhotonMapPathTracer():
    graph = RenderGraph("PhotonMapPathTracer")
    graph.create_pass("VBufferRT", "VBufferRT", {"samplePattern": "Stratified", "sampleCount": 16})
    graph.create_pass("PhotonMapPathTracer", "PhotonMapPathTracer")
    graph.create_pass("AccumulatePass", "AccumulatePass", {"enabled": False, "precisionMode": "SingleCompensated"})
    graph.create_pass("ToneMapper", "ToneMapper", {"autoExposure": False, "exposureCompensation": 0.0})
    graph.add_edge("VBufferRT.vbuffer", "PhotonMapPathTracer.vbuffer")
    graph.add_edge("VBufferRT.viewW", "PhotonMapPathTracer.viewW")
    graph.add_edge("PhotonMapPathTracer.color", "AccumulatePass.input")
    graph.add_edge("AccumulatePass.output", "ToneMapper.src")
    graph.mark_output("ToneMapper.dst")
    return graph


if "m" in globals():
    m.addGraph(render_graph_PhotonMapPathTracer())
