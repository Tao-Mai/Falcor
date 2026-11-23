from falcor import RenderGraph


def render_graph_PG5DTree():
    graph = RenderGraph("PG5DTree")
    graph.create_pass("VBufferRT", "VBufferRT", {"samplePattern": "Stratified", "sampleCount": 16})
    graph.create_pass("PG5DTree", "PG5DTree", {"maxBounces": 6, "enableGuiding": True, "enableControlVariate": False})
    graph.create_pass("AccumulatePass", "AccumulatePass", {"enabled": False, "precisionMode": "SingleCompensated"})
    graph.create_pass("ToneMapper", "ToneMapper", {"autoExposure": False, "exposureCompensation": 0.0})
    graph.add_edge("VBufferRT.vbuffer", "PG5DTree.vbuffer")
    graph.add_edge("VBufferRT.viewW", "PG5DTree.viewW")
    graph.add_edge("PG5DTree.color", "AccumulatePass.input")
    graph.add_edge("AccumulatePass.output", "ToneMapper.src")
    graph.mark_output("ToneMapper.dst")
    return graph


PG5DTree = render_graph_PG5DTree()
if "m" in globals():
    m.addGraph(PG5DTree)
