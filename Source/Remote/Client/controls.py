"""Local controls for Falcor graph and scene data."""

import copy
import json
import posixpath

import glfw
from imgui_bundle import imgui, imgui_node_editor as nodes


COMMIT = imgui.InputTextFlags_.enter_returns_true
CHILD_BORDER = imgui.ChildFlags_.borders if hasattr(imgui.ChildFlags_, "borders") else imgui.ChildFlags_.border


def menu_item(name, shortcut="", selected=False, enabled=True):
    return imgui.menu_item(name, shortcut, selected, enabled)[0]


def choose(label, value, choices):
    if not choices:
        return False, value
    index = choices.index(value) if value in choices else 0
    changed, index = imgui.combo(label, index, choices)
    return changed, choices[index]


def edit_value(label, value):
    if isinstance(value, bool):
        return imgui.checkbox(label, value)
    if isinstance(value, int) and -(2**31) <= value < 2**31:
        return imgui.input_int(label, value, step=0, flags=COMMIT)
    if isinstance(value, float):
        return imgui.input_float(label, value, format="%.5g", flags=COMMIT)
    if isinstance(value, str):
        return imgui.input_text(label, value, flags=COMMIT)
    changed, text = imgui.input_text(label, json.dumps(value), flags=COMMIT)
    if changed:
        return True, json.loads(text)
    return False, value


class FilePicker:
    def __init__(self, editor, title, operation, default, purpose):
        self.editor = editor
        self.title = title
        self.operation = operation
        self.purpose = purpose
        self.path = default
        self.directory = None
        self.open = True
        self.first = True
        self.version = 0
        self.browse(posixpath.dirname(default) or ".")

    def browse(self, path):
        self.version += 1
        version = self.version

        def update(result):
            if self.open and version == self.version:
                self.directory = result

        self.editor.send("browse", update, path=path, purpose=self.purpose)

    def submit(self):
        if self.path.strip():
            self.editor.send(self.operation, path=self.path.strip())
            self.open = False

    def draw(self, viewport, height):
        if not self.open:
            return
        if self.first:
            imgui.open_popup(self.title)
            self.first = False
        draw_width = max(330, min(650, viewport[2] - 40))
        imgui.set_next_window_pos((viewport[0] + viewport[2] / 2, height / 2),
                                  imgui.Cond_.appearing, (0.5, 0.5))
        imgui.set_next_window_size((draw_width, 444), imgui.Cond_.appearing)
        opened, self.open = imgui.begin_popup_modal(self.title, self.open)
        if not opened:
            return
        accepted, self.path = imgui.input_text("Server path", self.path, flags=COMMIT)
        if imgui.button("Open directory"):
            self.browse(self.path or ".")
        if self.directory:
            imgui.same_line()
            if imgui.button("Up"):
                self.browse(self.directory["parent"])
            imgui.text_wrapped(self.directory["path"])
            imgui.begin_child("Server files", (0, 270), CHILD_BORDER)
            for folder in self.directory["folders"]:
                if imgui.selectable(folder + "/", False)[0]:
                    self.browse(posixpath.join(self.directory["path"], folder))
            for name in self.directory["files"]:
                path = posixpath.join(self.directory["path"], name)
                if imgui.selectable(name, path == self.path)[0]:
                    self.path = path
                    if imgui.is_mouse_double_clicked(0):
                        accepted = True
            imgui.end_child()
        if accepted or imgui.button("Apply"):
            self.submit()
        imgui.same_line()
        if imgui.button("Cancel"):
            self.open = False
        if not self.open:
            imgui.close_current_popup()
        imgui.end_popup()


class GraphEditor:
    def __init__(self, editor, description):
        self.editor = editor
        self.graph = copy.deepcopy(description)
        self.preview = self.graph.get("preview", "")
        self.open = True
        config = nodes.Config()
        config.settings_file = ""
        self.context = nodes.create_editor(config)
        self.ids = {}
        self.placed = set()
        self.selection = ""
        self.catalog = []
        self.pass_index = 0
        self.pass_name = ""
        self.editor.send("catalog", self.set_catalog)

    def set_catalog(self, catalog):
        self.catalog = sorted(item["type"] for item in catalog)

    def get_id(self, key):
        if key not in self.ids:
            self.ids[key] = len(self.ids) + 1
        return self.ids[key]

    def close(self):
        nodes.destroy_editor(self.context)

    def add_pass(self):
        if not self.catalog:
            return
        kind = self.catalog[self.pass_index]
        name = self.pass_name.strip() or kind
        existing = {item["name"] for item in self.graph["passes"]}
        base = name
        suffix = 2
        while name in existing:
            name = f"{base}_{suffix}"
            suffix += 1

        def added(result):
            if self.open:
                self.graph["passes"].append(result)
                self.selection = result["name"]
                self.pass_name = ""

        self.editor.send("describe_pass", added, type=kind, name=name)

    def remove_pass(self, name):
        self.graph["passes"][:] = [item for item in self.graph["passes"] if item["name"] != name]
        self.graph["edges"][:] = [
            edge for edge in self.graph["edges"] if all(pin.split(".", 1)[0] != name for pin in edge)
        ]
        self.graph["outputs"][:] = [
            item for item in self.graph["outputs"] if item["name"].split(".", 1)[0] != name
        ]
        if self.selection == name:
            self.selection = ""

    def apply(self):
        description = {
            "name": self.graph["name"],
            "passes": [{key: item[key] for key in ("name", "type", "properties")} for item in self.graph["passes"]],
            "edges": self.graph["edges"],
            "outputs": self.graph["outputs"],
        }

        def applied(state):
            self.editor.set_state(state)
            self.open = False

        self.editor.send("apply_graph", applied, graph=description, preview=self.preview)

    def canvas(self, width, height):
        if width < 10 or height < 10:
            return
        nodes.set_current_editor(self.context)
        nodes.begin("Falcor graph", (width, height))
        pins = {}
        links = {}
        names = {}
        for index, item in enumerate(self.graph["passes"]):
            name = item["name"]
            identifier = nodes.NodeId(self.get_id(("node", name)))
            names[identifier.id()] = name
            if name not in self.placed:
                nodes.set_node_position(identifier, (20 + index % 3 * 235, 20 + index // 3 * 360))
                self.placed.add(name)
            nodes.begin_node(identifier)
            imgui.text(name)
            imgui.text_disabled(item["type"])
            for kind, label, fields in (
                (nodes.PinKind.input, "In", item["inputs"]),
                (nodes.PinKind.output, "Out", item["outputs"]),
            ):
                for field in ["", *fields]:
                    pin_name = name + "." + field if field else name
                    pin = nodes.PinId(self.get_id((label, pin_name)))
                    pins[pin.id()] = (kind, pin_name)
                    nodes.begin_pin(pin, kind)
                    imgui.text(f"{label}  {field or 'Execute'}")
                    nodes.end_pin()
            nodes.end_node()

        source_ids = {name: identifier for identifier, (kind, name) in pins.items() if kind == nodes.PinKind.output}
        target_ids = {name: identifier for identifier, (kind, name) in pins.items() if kind == nodes.PinKind.input}
        for edge in self.graph["edges"]:
            src, dst = edge
            if src in source_ids and dst in target_ids:
                link = nodes.LinkId(self.get_id(("link", src, dst)))
                links[link.id()] = edge
                nodes.link(link, nodes.PinId(source_ids[src]), nodes.PinId(target_ids[dst]))

        if nodes.begin_create():
            first, second = nodes.PinId(), nodes.PinId()
            if nodes.query_new_link(first, second) and first.id() in pins and second.id() in pins:
                (first_kind, first_name), (second_kind, second_name) = pins[first.id()], pins[second.id()]
                src, dst = (first_name, second_name) if first_kind == nodes.PinKind.output else (second_name, first_name)
                valid = (first_kind != second_kind and src.split(".")[0] != dst.split(".")[0]
                         and bool("." in src) == bool("." in dst)
                         and [src, dst] not in self.graph["edges"])
                if valid:
                    if nodes.accept_new_item():
                        self.graph["edges"].append([src, dst])
                else:
                    nodes.reject_new_item()
            nodes.end_create()

        if nodes.begin_delete():
            link = nodes.LinkId()
            while nodes.query_deleted_link(link):
                if nodes.accept_deleted_item() and link.id() in links:
                    self.graph["edges"].remove(links.pop(link.id()))
            node = nodes.NodeId()
            while nodes.query_deleted_node(node):
                if nodes.accept_deleted_item() and node.id() in names:
                    self.remove_pass(names[node.id()])
            nodes.end_delete()

        if nodes.get_selected_object_count():
            for node_id, name in names.items():
                if nodes.is_node_selected(nodes.NodeId(node_id)):
                    self.selection = name
                    break
        nodes.end()

    def draw(self, viewport, height):
        if not self.open:
            return
        imgui.set_next_window_pos((viewport[0] + 16, viewport[1] + 38), imgui.Cond_.appearing)
        imgui.set_next_window_size((max(440, viewport[2] - 32), max(380, min(height - 54, viewport[2] - 54))),
                                   imgui.Cond_.appearing)
        visible, self.open = imgui.begin("Render Graph Editor", self.open)
        if visible:
            _, self.graph["name"] = imgui.input_text("Graph name", self.graph["name"])
            if imgui.button("Apply graph"):
                self.apply()
            imgui.same_line()
            if imgui.button("Cancel"):
                self.open = False
            if self.catalog:
                imgui.set_next_item_width(220)
                _, self.pass_index = imgui.combo("##Pass type", self.pass_index, self.catalog)
                imgui.same_line()
                imgui.set_next_item_width(160)
                _, self.pass_name = imgui.input_text_with_hint("##Pass name", "Pass name", self.pass_name)
                imgui.same_line()
                if imgui.button("Add pass"):
                    self.add_pass()
            available = imgui.get_content_region_avail()
            detail_width = min(272, available.x * 0.34)
            self.canvas(available.x - detail_width - 10, available.y)
            imgui.same_line()
            imgui.begin_child("Pass settings", (detail_width, available.y), CHILD_BORDER)
            self.draw_selection()
            imgui.end_child()
        imgui.end()

    def draw_selection(self):
        item = next((item for item in self.graph["passes"] if item["name"] == self.selection), None)
        if not item:
            imgui.text_disabled("Select a pass to edit its settings and outputs.")
            return
        imgui.text(item["name"])
        if item.get("description"):
            imgui.text_wrapped(item["description"])
        if imgui.button("Remove selected pass"):
            self.remove_pass(item["name"])
            return
        for name, value in item["properties"].items():
            try:
                changed, new = edit_value(name, value)
                if changed:
                    item["properties"][name] = new
            except ValueError as error:
                self.editor.message = str(error)
        if item["outputs"]:
            imgui.separator_text("Graph outputs")
        for field in item["outputs"]:
            name = item["name"] + "." + field
            marked = next((output for output in self.graph["outputs"] if output["name"] == name), None)
            changed, selected = imgui.checkbox(field, marked is not None)
            if changed:
                if selected:
                    self.graph["outputs"].append({"name": name, "masks": [7]})
                else:
                    self.graph["outputs"].remove(marked)
        for edge in list(self.graph["edges"]):
            if any(name.split(".", 1)[0] == item["name"] for name in edge):
                imgui.push_id(" → ".join(edge))
                if imgui.small_button("Disconnect"):
                    self.graph["edges"].remove(edge)
                imgui.same_line()
                imgui.text_wrapped(" → ".join(edge))
                imgui.pop_id()


class Editor:
    def __init__(self, connection):
        self.connection = connection
        self.state = {}
        self.stats = {}
        self.message = ""
        self.dirty = True
        self.show_panel = True
        self.show_fps = True
        self.show_time = False
        self.show_profiler = False
        self.show_capture = False
        self.show_console = False
        self.show_help = False
        self.show_size = False
        self.show_all_outputs = False
        self.console = ""
        self.console_code = ""
        self.next_poll = 0.0
        self.video_fps = 0.0
        self.video_count = 0
        self.video_start = 0.0
        self.picker = None
        self.graph_editor = None
        self.capture_file = "Falcor.png"
        self.capture_count = 1
        self.capture_output = "Preview"

    def close(self):
        if self.graph_editor:
            self.graph_editor.close()

    def set_state(self, state):
        self.state = state
        self.set_stats(state.get("stats", {}))

    def send(self, op, completed=None, **values):
        def receive(result, error):
            if error:
                self.message = error
            elif completed is not None:
                completed(result)
            else:
                self.set_state(result)
            self.dirty = True

        try:
            self.connection.request(op, receive, **values)
        except ValueError as error:
            self.message = str(error)
        self.dirty = True

    def refresh(self):
        self.send("state")

    def poll_stats(self, now):
        if now >= self.next_poll and not self.connection.pending:
            self.next_poll = now + 0.5
            self.send("stats", self.set_stats)

    def set_stats(self, stats):
        if stats.get("error") and self.stats.get("error") != stats["error"]:
            self.message = stats["error"]
        self.stats = stats

    def received_frame(self, now):
        if not self.video_start:
            self.video_start = now
        self.video_count += 1
        if now - self.video_start >= 1.0:
            self.video_fps = self.video_count / (now - self.video_start)
            self.video_count = 0
            self.video_start = now

    def open_file(self, operation):
        title, default, purpose = {
            "load_script": ("Load Script", "scripts/", "script"),
            "load_scene": ("Load Scene", "media/", "scene"),
            "save_config": ("Save Config", "Falcor.py", "script"),
            "capture": ("Capture Frame", "Falcor.png", ""),
            "environment": ("Load Environment Map", "media/", ""),
            "profile_save": ("Save Profile", "Falcor-profile.json", ""),
        }[operation]
        self.picker = FilePicker(self, title, operation, default, purpose)

    def open_graph_editor(self, graph):
        if self.graph_editor:
            self.graph_editor.close()
        self.graph_editor = GraphEditor(self, graph)

    def shortcut(self, key, mods):
        if key == glfw.KEY_ESCAPE and self.graph_editor:
            self.graph_editor.open = False
        elif key == glfw.KEY_O and mods & glfw.MOD_CONTROL:
            self.open_file("load_scene" if mods & glfw.MOD_SHIFT else "load_script")
        elif key == glfw.KEY_S and mods & glfw.MOD_CONTROL:
            self.open_file("save_config")
        elif key == glfw.KEY_F1:
            self.show_help = not self.show_help
        elif key in (glfw.KEY_F2, glfw.KEY_F6):
            self.show_panel = not self.show_panel
        elif key == glfw.KEY_F5:
            self.send("reload_shaders")
        elif key == glfw.KEY_F7:
            self.send("overlay", enabled=not self.state.get("overlay", False))
        elif key == glfw.KEY_F9:
            self.show_time = not self.show_time
        elif key == glfw.KEY_F10:
            self.show_fps = not self.show_fps
        elif key == glfw.KEY_F12:
            self.open_file("capture")
        elif key == glfw.KEY_GRAVE_ACCENT:
            self.show_console = not self.show_console
        elif key == glfw.KEY_SPACE and mods & glfw.MOD_CONTROL:
            self.send("render_pause", paused=not self.state.get("render_paused", False))
        elif key == glfw.KEY_SPACE:
            self.send("clock", changes={"paused": not self.state.get("clock", {}).get("paused", False)})
        else:
            return False
        self.dirty = True
        return True

    def draw(self, width, height, minimum_sidebar):
        self.dirty = False
        menu_height = min(height, self.draw_menu())
        side = min(width - minimum_sidebar, height - menu_height)
        viewport = (0, menu_height, side, side)
        if self.show_panel and width > side:
            self.draw_panel(width, height, width - side, menu_height)
        if self.show_time:
            self.draw_time(viewport)
        if self.show_console:
            self.draw_console(viewport, height)
        if self.show_profiler:
            self.draw_profiler(viewport)
        if self.show_capture:
            self.draw_capture(viewport)
        if self.show_size:
            self.draw_size()
        if self.show_help:
            self.draw_help()
        if self.graph_editor:
            self.graph_editor.draw(viewport, height)
            if not self.graph_editor.open:
                self.graph_editor.close()
                self.graph_editor = None
        if self.picker:
            self.picker.draw(viewport, height)
            if not self.picker.open:
                self.picker = None
        if self.message:
            self.draw_message()
        return viewport

    def draw_menu(self):
        if not imgui.begin_main_menu_bar():
            return 0
        height = round(imgui.get_window_height())
        if imgui.begin_menu("File"):
            if menu_item("Load Script", "Ctrl+O"):
                self.open_file("load_script")
            if menu_item("Load Scene", "Ctrl+Shift+O"):
                self.open_file("load_scene")
            if menu_item("Save Config", "Ctrl+S"):
                self.open_file("save_config")
            if menu_item("Capture Frame", "F12"):
                self.open_file("capture")
            if menu_item("Unload Scene", enabled=self.state.get("scene") is not None):
                self.send("unload_scene")
            for title, operation, kind in (("Recent Scripts", "load_script", "scripts"), ("Recent Scenes", "load_scene", "scenes")):
                paths = self.state.get("recent_" + kind, [])
                if paths and imgui.begin_menu(title):
                    for path in paths:
                        if menu_item(path):
                            self.send(operation, path=path)
                    imgui.end_menu()
            imgui.end_menu()
        if imgui.begin_menu("View"):
            for label, shortcut, item in (
                ("Graphs", "F6", "show_panel"), ("FPS", "F10", "show_fps"), ("Time", "F9", "show_time"),
                ("Console", "`", "show_console"), ("Profiler", "", "show_profiler"), ("Render Size", "", "show_size"),
                ("Frame Capture", "", "show_capture"),
            ):
                value = getattr(self, item)
                if menu_item(label, shortcut, value):
                    setattr(self, item, not value)
            if menu_item("Graph Overlays", "F7", self.state.get("overlay", False)):
                self.send("overlay", enabled=not self.state.get("overlay", False))
            if menu_item("Pause Rendering", "Ctrl+Space", self.state.get("render_paused", False)):
                self.send("render_pause", paused=not self.state.get("render_paused", False))
            if menu_item("Reload Shaders", "F5"):
                self.send("reload_shaders")
            imgui.end_menu()
        if imgui.begin_menu("Help"):
            if menu_item("Shortcuts", "F1"):
                self.show_help = True
            imgui.end_menu()
        if self.show_fps:
            fps = f"Video {self.video_fps:.1f} FPS   Render {self.stats.get('render_fps', 0):.1f} FPS"
            right = imgui.get_window_width() - imgui.calc_text_size(fps).x - 12
            if right >= imgui.get_cursor_pos_x():
                imgui.set_cursor_pos_x(right)
                imgui.align_text_to_frame_padding()
                imgui.text_disabled(fps)
        imgui.end_main_menu_bar()
        return height

    def draw_panel(self, width, height, sidebar, menu_height):
        imgui.set_next_window_pos((width - sidebar, menu_height), imgui.Cond_.always)
        imgui.set_next_window_size((sidebar, max(1, height - menu_height)), imgui.Cond_.always)
        visible, _ = imgui.begin("Graphs", flags=(imgui.WindowFlags_.no_move | imgui.WindowFlags_.no_resize |
                                                 imgui.WindowFlags_.no_collapse | imgui.WindowFlags_.no_saved_settings))
        if visible:
            graph = self.state.get("graph")
            if graph:
                self.draw_graph(graph)
            elif self.state:
                imgui.text_disabled("Load or create a render graph.")
            else:
                imgui.text_disabled("Waiting for renderer state…")
            if imgui.button("New Graph"):
                self.open_graph_editor({"name": "RenderGraph", "passes": [], "edges": [], "outputs": []})
            if self.state.get("scene") and imgui.collapsing_header("Scene Settings"):
                self.draw_scene(self.state["scene"])
            if graph and imgui.collapsing_header("Render Passes", imgui.TreeNodeFlags_.default_open):
                self.draw_passes(graph)
            if imgui.collapsing_header("Framework Stats"):
                self.draw_metrics()
        imgui.end()

    def draw_graph(self, graph):
        changed, selected = choose("Active Graph", graph["name"], self.state.get("graphs", []))
        if changed:
            self.send("activate_graph", name=selected)
        if imgui.button("Edit"):
            self.open_graph_editor(graph)
        imgui.same_line()
        if imgui.button("Remove"):
            imgui.open_popup("Remove graph?")
        opened, _ = imgui.begin_popup_modal("Remove graph?")
        if opened:
            imgui.text_wrapped(f"Remove {graph['name']} from the renderer?")
            if imgui.button("Remove Graph"):
                self.send("remove_graph", name=graph["name"])
                imgui.close_current_popup()
            imgui.same_line()
            if imgui.button("Cancel"):
                imgui.close_current_popup()
            imgui.end_popup()
        _, self.show_all_outputs = imgui.checkbox("List All Outputs", self.show_all_outputs)
        choices = graph["available"] if self.show_all_outputs else [item["name"] for item in graph["outputs"]]
        changed, output = choose("Output", graph.get("preview", ""), choices)
        if changed:
            self.send("preview_output", name=output)
        if imgui.button("Capture Current Output"):
            self.open_file("capture")
        imgui.same_line()
        if imgui.button("Frame Capture"):
            self.show_capture = True
        imgui.separator()

    def draw_passes(self, graph):
        for item in graph["passes"]:
            imgui.push_id(item["name"])
            if imgui.tree_node(item["name"]):
                if item.get("description"):
                    imgui.text_wrapped(item["description"])
                for label, value in item.get("properties", {}).items():
                    try:
                        changed, new = edit_value(label, value)
                        if changed:
                            self.send("pass_properties", graph=graph["name"], name=item["name"], properties={label: new})
                    except ValueError as error:
                        self.message = str(error)
                imgui.tree_pop()
            imgui.pop_id()

    def draw_scene(self, scene):
        imgui.text_wrapped(scene["path"])
        changed, camera = choose("Selected Camera", scene["camera"], scene["cameras"])
        if changed:
            self.send("scene", changes={"camera": camera})
        changed, controller = imgui.combo("Camera Controller", scene["controller"], ["First Person", "Orbiter", "Six DOF"])
        if changed:
            self.send("scene", changes={"controller": controller})
        for name, label in (
            ("camera_speed", "Camera Speed"), ("animated", "Animate Scene"), ("env_light", "Environment Light"),
            ("analytic_lights", "Analytic Lights"), ("emissive_lights", "Emissive Lights"),
            ("grid_volumes", "Grid Volumes"), ("albedo_multiplier", "Albedo Multiplier"),
        ):
            changed, value = edit_value(label, scene[name])
            if changed:
                self.send("scene", changes={name: value})
        if imgui.tree_node("Camera"):
            for name, label in (("position", "Position"), ("target", "Target"), ("up", "Up")):
                changed, value = imgui.input_float3(label, scene[name], flags=COMMIT)
                if changed:
                    self.send("scene", changes={name: list(value)})
            for name, label in (("focal_length", "Focal Length"), ("aperture", "Aperture"), ("near", "Near"), ("far", "Far")):
                changed, value = edit_value(label, scene[name])
                if changed:
                    self.send("scene", changes={name: value})
            imgui.tree_pop()
        if imgui.button("Add Viewpoint"):
            self.send("viewpoint", action="add")
        imgui.same_line()
        if imgui.button("Remove Viewpoint"):
            self.send("viewpoint", action="remove")
        if scene.get("viewpoint_count", 0) > 1:
            choices = ["Default Viewpoint", *[f"Viewpoint {i}" for i in range(1, scene["viewpoint_count"])]]
            changed, index = imgui.combo("Viewpoint", scene["viewpoint"], choices)
            if changed:
                self.send("viewpoint", action="select", index=index)
        if imgui.button("Load Environment Map"):
            self.open_file("environment")
        imgui.same_line()
        if imgui.button("Clear Environment"):
            self.send("environment", path="")

    def draw_metrics(self):
        for key, label in (("submit_ms", "CPU commands"), ("gpu_wait_ms", "GPU and copy"), ("encode_ms", "NVENC")):
            imgui.text(f"{label}  {self.stats.get(key, 0):.2f} ms")
        imgui.text(f"Encoded stream  {self.stats.get('fps', 0):.1f} FPS")
        imgui.text(f"Stream limit  {self.stats.get('limit', 60)} FPS")
        imgui.text(f"Rendered frames  {self.stats.get('frame', 0)}")

    def draw_time(self, viewport):
        imgui.set_next_window_pos((viewport[0] + 24, viewport[1] + 58), imgui.Cond_.appearing)
        imgui.set_next_window_size((355, 0), imgui.Cond_.appearing)
        visible, self.show_time = imgui.begin("Time", self.show_time)
        if visible:
            clock = self.state.get("clock", {})
            changed, value = imgui.input_float("Time", self.stats.get("time", 0.0), format="%.3f", flags=COMMIT)
            if changed:
                self.send("clock", changes={"time": value})
            for key, label in (("scale", "Scale"), ("fps", "Simulation FPS")):
                changed, value = edit_value(label, clock.get(key, 0))
                if changed and value >= 0:
                    self.send("clock", changes={key: value})
            paused = clock.get("paused", False)
            if imgui.button("Play" if paused else "Pause"):
                self.send("clock", changes={"paused": not paused})
            imgui.same_line()
            if imgui.button("Rewind"):
                self.send("clock", changes={"time": 0.0})
            for count, label in ((-1, "Previous Frame"), (1, "Next Frame")):
                if imgui.button(label):
                    self.send("clock", changes={"step": count})
                if count < 0:
                    imgui.same_line()
        imgui.end()

    def draw_console(self, viewport, height):
        width = max(325, viewport[2] - 28)
        imgui.set_next_window_pos((viewport[0] + 14, max(30, height - 298)), imgui.Cond_.appearing)
        imgui.set_next_window_size((width, 280), imgui.Cond_.appearing)
        visible, self.show_console = imgui.begin("Python Console", self.show_console)
        if visible:
            imgui.input_text_multiline("##Console output", self.console, (-1, 110), flags=imgui.InputTextFlags_.read_only)
            _, self.console_code = imgui.input_text_multiline("##Python code", self.console_code, (-1, 70))
            if imgui.button("Run") and self.console_code.strip():
                code = self.console_code

                def executed(result):
                    self.console = (self.console + ">>> " + code + "\n" + result["output"])[-24000:]
                    self.set_state(result["state"])

                self.send("console", executed, code=code)
            imgui.same_line()
            if imgui.button("Clear"):
                self.console = ""
        imgui.end()

    def draw_profiler(self, viewport):
        imgui.set_next_window_pos((viewport[0] + 40, viewport[1] + 118), imgui.Cond_.appearing)
        imgui.set_next_window_size((530, 380), imgui.Cond_.appearing)
        visible, self.show_profiler = imgui.begin("Profiler", self.show_profiler)
        if visible:
            self.draw_metrics()
            changed, enabled = imgui.checkbox("Enable Falcor profiler", self.state.get("profile_active", False))
            if changed:
                self.send("profile", enabled=enabled)
            if self.state.get("profile_recording", False):
                if imgui.button("Stop and Save Capture"):
                    self.open_file("profile_save")
            elif imgui.button("Start Capture"):
                self.send("profile_start")
            imgui.separator()
            for event in self.stats.get("events", []):
                imgui.text_wrapped(f"{event['name']}   CPU {event['cpu_ms']:.2f}   GPU {event['gpu_ms']:.2f} ms")
        imgui.end()

    def draw_size(self):
        visible, self.show_size = imgui.begin("Render Size", self.show_size, imgui.WindowFlags_.always_auto_resize)
        if visible:
            changed, values = imgui.input_int2("Render Width / Height", self.state.get("render_size", [1080, 1080]), flags=COMMIT)
            if changed:
                self.send("render_size", width=values[0], height=values[1])
            imgui.text_disabled("Preview video 1080 × 1080")
        imgui.end()

    def draw_capture(self, viewport):
        imgui.set_next_window_pos((viewport[0] + 42, viewport[1] + 130), imgui.Cond_.appearing)
        imgui.set_next_window_size((510, 0), imgui.Cond_.appearing)
        visible, self.show_capture = imgui.begin("Frame Capture", self.show_capture, imgui.WindowFlags_.always_auto_resize)
        if visible:
            graph = self.state.get("graph") or {}
            outputs = ["Preview", *[item["name"] for item in graph.get("outputs", [])]]
            if self.capture_output not in outputs:
                self.capture_output = "Preview"
            _, self.capture_output = choose("Output", self.capture_output, outputs)
            _, self.capture_file = imgui.input_text("Server filename", self.capture_file)
            _, self.capture_count = imgui.input_int("Frame count", self.capture_count)
            output = "" if self.capture_output == "Preview" else self.capture_output
            if self.stats.get("captured", 0):
                imgui.text(f"Captured {self.stats['captured']} frames")
            if self.stats.get("capturing", False):
                if imgui.button("Stop Capture"):
                    self.send("capture_stop")
            else:
                if imgui.button("Capture One") and self.capture_file:
                    self.send("capture", path=self.capture_file, output=output)
                imgui.same_line()
                if imgui.button("Capture Sequence") and self.capture_file and self.capture_count > 0:
                    self.send("capture_start", path=self.capture_file, output=output, count=self.capture_count)
        imgui.end()

    def draw_help(self):
        visible, self.show_help = imgui.begin("Shortcuts", self.show_help, imgui.WindowFlags_.always_auto_resize)
        if visible:
            for item in (
                "Hold right mouse in the viewport to look.",
                "WASD moves, Q/E descends or ascends.",
                "F5 reloads shaders. F6 toggles the controls.",
                "F7 toggles graph overlays. F9 shows time.",
                "F10 toggles FPS. F12 captures a frame.",
                "Space pauses the clock. Ctrl+Space pauses rendering.",
            ):
                imgui.text(item)
        imgui.end()

    def draw_message(self):
        visible, opened = imgui.begin("Renderer Message", True)
        if visible:
            imgui.text_wrapped(self.message[:7000])
            if imgui.button("Dismiss"):
                opened = False
        imgui.end()
        if not opened:
            self.message = ""
