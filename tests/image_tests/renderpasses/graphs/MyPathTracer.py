from falcor import *

def render_graph_MyPathTracer():
    g = RenderGraph("MyPathTracer")
    MyPathTracer = createPass("MyPathTracer")
    g.addPass(MyPathTracer, "MyPathTracer")
    VBufferRT = createPass("VBufferRT")
    g.addPass(VBufferRT, "VBufferRT")
    AccumulatePass = createPass("AccumulatePass")
    g.addPass(AccumulatePass, "AccumulatePass")
    ToneMapper = createPass("ToneMapper", {'autoExposure': False, 'operator': 'Linear', 'clamp': False, 'outputFormat': 'RGBA32Float'})
    g.addPass(ToneMapper, "ToneMapper")
    g.addEdge("VBufferRT.vbuffer", "MyPathTracer.vbuffer")
    g.addEdge("VBufferRT.viewW", "MyPathTracer.viewW")
    g.addEdge("MyPathTracer.color", "AccumulatePass.input")
    g.addEdge("AccumulatePass.output", "ToneMapper.src")
    g.markOutput("ToneMapper.dst")
    g.markOutput("ToneMapper.dst", TextureChannelFlags.Alpha)
    return g

MyPathTracer = render_graph_MyPathTracer()
try: m.addGraph(MyPathTracer)
except NameError: None
