from falcor import *

def render_graph_MinimalPathTracer():
    g = RenderGraph("MinimalPathTracer")
    ToneMapper = createPass("ToneMapper", {'autoExposure': False, 'exposureCompensation': 0.0})
    g.addPass(ToneMapper, "ToneMapper")
    MinimalPathTracer = createPass("MinimalPathTracer", {'maxBounces': 6})
    g.addPass(MinimalPathTracer, "MinimalPathTracer")
    VBufferRT = createPass("VBufferRT", {'samplePattern': 'Stratified', 'sampleCount': 16})
    g.addPass(VBufferRT, "VBufferRT")
    g.addEdge("VBufferRT.vbuffer", "MinimalPathTracer.vbuffer")
    g.addEdge("VBufferRT.viewW", "MinimalPathTracer.viewW")
    g.addEdge("MinimalPathTracer.color", "ToneMapper.src")
    g.markOutput("ToneMapper.dst")
    return g

MinimalPathTracer = render_graph_MinimalPathTracer()
try: m.addGraph(MinimalPathTracer)
except NameError: None
