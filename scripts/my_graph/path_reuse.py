from pathlib import Path

from falcor import RenderGraph


def render_graph_PathReuse():
    graph = RenderGraph("PathReuse")
    graph.create_pass("VBufferRT", "VBufferRT", {"samplePattern": "Stratified", "sampleCount": 16})
    graph.create_pass("PathReuse", "PathReuse", {"maxBounces": 6, "grouping": "N-rooks"})
    graph.create_pass("ToneMapper", "ToneMapper", {"autoExposure": False, "exposureCompensation": 0.0})
    graph.add_edge("VBufferRT.vbuffer", "PathReuse.vbuffer")
    graph.add_edge("VBufferRT.viewW", "PathReuse.viewW")
    graph.add_edge("PathReuse.color", "ToneMapper.src")
    graph.mark_output("ToneMapper.dst")
    return graph


PathReuse = render_graph_PathReuse()
if "m" in globals():
    m.addGraph(PathReuse)
    scripts_dir = Path(__file__).resolve().parents[1]
    m.script(str(scripts_dir / "MyPathTracer.py"))
    m.script(str(scripts_dir / "PG5DTree.py"))
