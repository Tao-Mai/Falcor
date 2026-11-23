from falcor import *


def render_graph_MyPathTracer(enable_accumulation=False):
    graph = RenderGraph("MyPathTracer")
    graph.addPass(createPass("VBufferRT", {"samplePattern": "Stratified", "sampleCount": 16}), "VBufferRT")
    graph.addPass(createPass("MyPathTracer", {"maxBounces": 6}), "MyPathTracer")
    graph.addPass(
        createPass("AccumulatePass", {"enabled": enable_accumulation, "precisionMode": "SingleCompensated"}),
        "AccumulatePass",
    )
    graph.addPass(createPass("ToneMapper", {"autoExposure": False, "exposureCompensation": 0.0}), "ToneMapper")
    graph.addEdge("VBufferRT.vbuffer", "MyPathTracer.vbuffer")
    graph.addEdge("VBufferRT.viewW", "MyPathTracer.viewW")
    graph.addEdge("MyPathTracer.color", "AccumulatePass.input")
    graph.addEdge("AccumulatePass.output", "ToneMapper.src")
    graph.markOutput("ToneMapper.dst")
    return graph


MyPathTracer = render_graph_MyPathTracer(enable_accumulation=False)
if "m" in globals():
    m.addGraph(MyPathTracer)
