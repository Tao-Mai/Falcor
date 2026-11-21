from falcor import *


def render_graph_PathTracerRTXDI():
    g = RenderGraph("PathTracerRTXDI")
    g.createPass("VBufferRT", "VBufferRT", {"samplePattern": "Center", "useDOF": False})
    g.createPass("PathTracer", "PathTracer", {
        "useRTXDI": True, "useReSTIRPT": False, "samplesPerPixel": 1, "colorFormat": "RGBA32F",
        "maxSurfaceBounces": 0, "maxDiffuseBounces": 0, "maxSpecularBounces": 0, "maxTransmissionBounces": 0,
    })
    g.createPass("AccumulatePass", "AccumulatePass", {"enabled": True, "precisionMode": "Single"})
    g.createPass("ToneMapper", "ToneMapper", {"autoExposure": False, "exposureCompensation": 0.0})
    g.addEdge("VBufferRT.vbuffer", "PathTracer.vbuffer")
    g.addEdge("VBufferRT.viewW", "PathTracer.viewW")
    g.addEdge("VBufferRT.mvec", "PathTracer.mvec")
    g.addEdge("PathTracer.color", "AccumulatePass.input")
    g.addEdge("AccumulatePass.output", "ToneMapper.src")
    g.markOutput("ToneMapper.dst")
    return g


PathTracerRTXDI = render_graph_PathTracerRTXDI()
try:
    m.addGraph(PathTracerRTXDI)
except NameError:
    pass
