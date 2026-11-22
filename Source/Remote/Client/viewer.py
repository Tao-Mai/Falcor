"""Falcor's local editor and hardware-decoded remote scene viewport."""

import argparse
from collections import deque
import ctypes
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import sys
import time

if os.getenv("XDG_SESSION_TYPE") == "wayland":
    os.environ.setdefault("PYOPENGL_PLATFORM", "x11")

import glfw
from OpenGL import GL
from OpenGL.GL import shaders
import PyNvVideoCodec as nvc
import imgui_bundle
from imgui_bundle import imgui
from imgui_bundle.python_backends.glfw_backend import GlfwRenderer

from controls import Editor


FRAME_SIZE = struct.Struct("!I")
INPUT_EVENT = struct.Struct("!4I2f")
KEY, MOVE, BUTTON, SCROLL, RELEASE_ALL, LOOK = range(1, 7)
OPEN_PREVIEW, OPEN_CONTROLS = 11, 12
MOVEMENT_KEYS = (glfw.KEY_W, glfw.KEY_A, glfw.KEY_S, glfw.KEY_D, glfw.KEY_Q, glfw.KEY_E)
MAX_FRAME_BYTES = 16 * 1024 * 1024
MAX_CONTROL_BYTES = 1024 * 1024
VIEWPORT_SIDE = 1080
PANEL_WIDTH = 512
FONT_SIZE = 18.0
FRAME_PADDING = (8, 4)
WINDOW_WIDTH = VIEWPORT_SIDE + PANEL_WIDTH
WINDOW_HEIGHT = VIEWPORT_SIDE + int(FONT_SIZE + 2 * FRAME_PADDING[1])


FRAGMENT_SHADER = """#version 120
uniform sampler2D luma;
uniform sampler2D chroma;
void main() {
    vec2 pos = gl_TexCoord[0].st;
    float y = (texture2D(luma, pos).r * 255.0 - 16.0) / 219.0;
    vec2 uv = (texture2D(chroma, pos).ra * 255.0 - 128.0) / 224.0;
    gl_FragColor = vec4(y + 1.5748 * uv.y,
                        y - 0.1873 * uv.x - 0.4681 * uv.y,
                        y + 1.8556 * uv.x, 1.0);
}
"""


class Connection:
    def __init__(self, sock):
        self.sock = sock
        self.buffer = bytearray()
        self.inputs = deque()
        self.input_offset = 0
        self.input_overflow = False
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.setblocking(False)

    def enqueue(self, kind, code=0, action=0, mods=0, x=0.0, y=0.0):
        data = INPUT_EVENT.pack(kind, code, action, mods, x, y)
        motion = kind if kind == MOVE or (kind == LOOK and action == glfw.REPEAT) else None
        can_replace = len(self.inputs) > 1 or (self.inputs and self.input_offset == 0)
        if motion is not None and can_replace and self.inputs[-1][0] == motion:
            self.inputs[-1] = (motion, data)
        elif len(self.inputs) < 256:
            self.inputs.append((motion, data))
        else:
            self.input_overflow = True

    def send_inputs(self):
        if self.input_overflow:
            raise ConnectionError("Could not send input fast enough")
        while self.inputs:
            _, data = self.inputs[0]
            try:
                sent = self.sock.send(data[self.input_offset:])
            except BlockingIOError:
                return
            if not sent:
                raise ConnectionError("The renderer closed the input connection")
            self.input_offset += sent
            if self.input_offset == len(data):
                self.inputs.popleft()
                self.input_offset = 0

    def receive_frames(self):
        read_socket = False
        for _ in range(4):
            while True:
                if len(self.buffer) >= FRAME_SIZE.size:
                    size, = FRAME_SIZE.unpack_from(self.buffer)
                    if not 0 < size <= MAX_FRAME_BYTES:
                        raise ValueError("Invalid H.264 packet size from the renderer")
                    end = FRAME_SIZE.size + size
                    if len(self.buffer) >= end:
                        payload = self.buffer[FRAME_SIZE.size:end]
                        del self.buffer[:end]
                        break
                if read_socket:
                    return
                try:
                    data = self.sock.recv(256 * 1024)
                except BlockingIOError:
                    return
                if not data:
                    raise ConnectionError("The renderer closed the video connection")
                self.buffer.extend(data)
                read_socket = True
            yield payload


class ControlConnection:
    def __init__(self, sock):
        self.sock = sock
        self.sock.setblocking(False)
        self.next_id = 0
        self.outgoing = deque()
        self.pending = {}
        self.input = bytearray()
        self.offset = 0

    def request(self, op, callback, **arguments):
        if len(self.pending) >= 64:
            callback(None, "The renderer already has pending operations.")
            return
        self.next_id += 1
        data = json.dumps(dict(op=op, id=self.next_id, **arguments), allow_nan=False).encode("utf-8")
        if len(data) > MAX_CONTROL_BYTES:
            callback(None, "The operation exceeds the control message size limit.")
            return
        self.outgoing.append(FRAME_SIZE.pack(len(data)) + data)
        self.pending[self.next_id] = callback

    def poll(self):
        while self.outgoing:
            packet = self.outgoing[0]
            try:
                count = self.sock.send(memoryview(packet)[self.offset:])
            except BlockingIOError:
                break
            if not count:
                raise ConnectionError("The renderer closed the control connection")
            self.offset += count
            if self.offset == len(packet):
                self.outgoing.popleft()
                self.offset = 0

        try:
            data = self.sock.recv(128 * 1024)
        except BlockingIOError:
            return
        if not data:
            raise ConnectionError("The renderer closed the control connection")
        self.input.extend(data)
        while len(self.input) >= FRAME_SIZE.size:
            size, = FRAME_SIZE.unpack_from(self.input)
            if not 0 < size <= MAX_CONTROL_BYTES:
                raise ValueError("Invalid control response size")
            end = FRAME_SIZE.size + size
            if len(self.input) < end:
                break
            response = json.loads(self.input[FRAME_SIZE.size:end])
            del self.input[:end]
            callback = self.pending.pop(response["id"], None)
            if callback:
                callback(response.get("result"), response.get("error"))


def panel_width(width):
    return min(PANEL_WIDTH, width * PANEL_WIDTH // WINDOW_WIDTH)


def configure_ui():
    imgui.style_colors_dark()
    style = imgui.get_style()
    style.window_padding = (12, 10)
    style.frame_padding = FRAME_PADDING
    style.item_spacing = (8, 6)
    style.frame_rounding = 3.0
    style.grab_rounding = 3.0
    style.popup_rounding = 4.0

    col = imgui.Col_
    for role, rgb in (
        (col.window_bg, (0.085, 0.090, 0.104)),
        (col.menu_bar_bg, (0.105, 0.115, 0.136)),
        (col.title_bg, (0.115, 0.130, 0.154)),
        (col.title_bg_active, (0.138, 0.157, 0.188)),
        (col.frame_bg, (0.144, 0.168, 0.205)),
        (col.button, (0.175, 0.215, 0.275)),
        (col.header, (0.164, 0.192, 0.235)),
    ):
        style.set_color_(role.value, imgui.ImVec4(*rgb, 1.0))

    io = imgui.get_io()
    io.set_ini_filename(None if hasattr(style, "font_scale_main") else "")
    atlas = io.fonts
    if sys.platform == "win32":
        fonts = Path(os.environ.get("WINDIR", r"C:\Windows")) / "Fonts"
        for name in ("msyh.ttc", "segoeui.ttf"):
            path = fonts / name
            if not path.is_file():
                continue
            try:
                if name == "msyh.ttc" and hasattr(atlas, "get_glyph_ranges_chinese_full"):
                    font = atlas.add_font_from_file_ttf(str(path), FONT_SIZE, None, atlas.get_glyph_ranges_chinese_full())
                else:
                    font = atlas.add_font_from_file_ttf(str(path), FONT_SIZE)
                if font is not None:
                    return
            except (OSError, RuntimeError):
                continue

    bundled_font = Path(imgui_bundle.__file__).parent / "assets/fonts/Roboto/Roboto-Regular.ttf"
    atlas.add_font_from_file_ttf(str(bundled_font), FONT_SIZE)


class Video:
    def __init__(self):
        self.width = self.height = 0
        self.reset_decoder()
        self.packet = nvc.PacketData()
        self.packet.decode_flag = nvc.VideoPacketFlag.ENDOFPICTURE
        self.textures = GL.glGenTextures(2)
        for texture in self.textures:
            GL.glBindTexture(GL.GL_TEXTURE_2D, int(texture))
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_LINEAR)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_LINEAR)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_S, GL.GL_CLAMP_TO_EDGE)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_T, GL.GL_CLAMP_TO_EDGE)

    def reset_decoder(self):
        self.decoder = nvc.CreateDecoder(
            codec=nvc.cudaVideoCodec.H264, usedevicememory=False,
            latency=nvc.DisplayDecodeLatencyType.ZERO,
        )

    def decode(self, data):
        self.packet.bsl = len(data)
        self.packet.bsl_data = ctypes.addressof(ctypes.c_char.from_buffer(data))
        frames = self.decoder.Decode(self.packet)
        if not frames:
            return False
        self.upload(frames[-1])
        return True

    def upload(self, frame):
        rows, width = frame.shape
        height = rows * 2 // 3
        resize = (self.width, self.height) != (width, height)
        self.width, self.height = width, height
        for index, (w, h, format_) in enumerate(
            ((width, height, GL.GL_LUMINANCE), (width // 2, height // 2, GL.GL_LUMINANCE_ALPHA))
        ):
            GL.glActiveTexture(GL.GL_TEXTURE0 + index)
            GL.glBindTexture(GL.GL_TEXTURE_2D, int(self.textures[index]))
            GL.glPixelStorei(GL.GL_UNPACK_ROW_LENGTH, frame.strides[0] // (index + 1))
            pixels = ctypes.c_void_p(frame.GetPtrToPlane(index))
            if resize:
                GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, format_, w, h, 0, format_, GL.GL_UNSIGNED_BYTE, pixels)
            else:
                GL.glTexSubImage2D(GL.GL_TEXTURE_2D, 0, 0, 0, w, h, format_, GL.GL_UNSIGNED_BYTE, pixels)
        GL.glPixelStorei(GL.GL_UNPACK_ROW_LENGTH, 0)

    def draw(self, rect):
        if not self.width or not rect[2] or not rect[3]:
            return
        for index, texture in enumerate(self.textures):
            GL.glActiveTexture(GL.GL_TEXTURE0 + index)
            GL.glBindTexture(GL.GL_TEXTURE_2D, int(texture))
        GL.glViewport(*rect)
        GL.glBegin(GL.GL_QUADS)
        GL.glTexCoord2f(0, 0)
        GL.glVertex2f(-1, 1)
        GL.glTexCoord2f(1, 0)
        GL.glVertex2f(1, 1)
        GL.glTexCoord2f(1, 1)
        GL.glVertex2f(1, -1)
        GL.glTexCoord2f(0, 1)
        GL.glVertex2f(-1, -1)
        GL.glEnd()


def run(window, video_connection, control_connection):
    imgui.create_context()
    configure_ui()
    backend = GlfwRenderer(window, attach_callbacks=False)
    editor = Editor(control_connection)
    video = Video()
    fragment = shaders.compileShader(FRAGMENT_SHADER, GL.GL_FRAGMENT_SHADER)
    program = shaders.compileProgram(fragment, validate=False)
    GL.glUseProgram(program)
    GL.glUniform1i(GL.glGetUniformLocation(program, "luma"), 0)
    GL.glUniform1i(GL.glGetUniformLocation(program, "chroma"), 1)
    GL.glPixelStorei(GL.GL_UNPACK_ALIGNMENT, 1)
    look_origin = None
    forwarded_keys = set()
    forwarded_buttons = set()
    redraw = True
    viewport = (0, 0, 0, 0)

    def position(x, y, dragging=False):
        left, top, side, _ = viewport
        if not side or not video.width:
            return None
        if dragging or (left <= x < left + side and top <= y < top + side):
            return (min(1.0, max(0.0, (x - left) / side)), min(1.0, max(0.0, (y - top) / side)))
        return None

    def cursor(_, x, y):
        nonlocal redraw
        redraw = True
        if look_origin is not None:
            start_x, start_y, side = look_origin
            video_connection.enqueue(LOOK, action=glfw.REPEAT, x=0.5 + (x - start_x) / side, y=0.5 + (y - start_y) / side)
        else:
            backend.mouse_callback()
            pos = position(x, y, bool(forwarded_buttons))
            if pos and (forwarded_buttons or not imgui.get_io().want_capture_mouse):
                video_connection.enqueue(MOVE, x=pos[0], y=pos[1])

    def on_key(_, key, scancode, action, mods):
        nonlocal redraw
        redraw = True
        if key == glfw.KEY_UNKNOWN:
            return
        if look_origin is None:
            backend.keyboard_callback(window, key, scancode, action, mods)
            if action == glfw.PRESS and not imgui.get_io().want_text_input and editor.shortcut(key, mods):
                return
        if action == glfw.RELEASE and key in forwarded_keys:
            forwarded_keys.remove(key)
        elif action == glfw.PRESS and (look_origin is not None or not imgui.get_io().want_capture_keyboard):
            if key in MOVEMENT_KEYS and look_origin is None:
                return
            forwarded_keys.add(key)
        else:
            return
        video_connection.enqueue(KEY, key, action, mods)

    def stop_look():
        nonlocal look_origin
        if look_origin is None:
            return
        for key in tuple(forwarded_keys):
            if key in MOVEMENT_KEYS:
                video_connection.enqueue(KEY, key, glfw.RELEASE)
                forwarded_keys.remove(key)
        video_connection.enqueue(LOOK, action=glfw.RELEASE)
        look_origin = None
        glfw.set_input_mode(window, glfw.CURSOR, glfw.CURSOR_NORMAL)

    def on_button(_, button, action, mods):
        nonlocal look_origin, redraw
        redraw = True
        if look_origin is not None:
            if button == glfw.MOUSE_BUTTON_RIGHT and action == glfw.RELEASE:
                stop_look()
            return
        x, y = glfw.get_cursor_pos(window)
        io = imgui.get_io()
        pos = position(x, y, dragging=button in forwarded_buttons)
        if button == glfw.MOUSE_BUTTON_RIGHT and action == glfw.PRESS and pos and not io.want_capture_mouse and not forwarded_buttons:
            glfw.set_input_mode(window, glfw.CURSOR, glfw.CURSOR_DISABLED)
            if glfw.raw_mouse_motion_supported():
                glfw.set_input_mode(window, glfw.RAW_MOUSE_MOTION, glfw.TRUE)
            start_x, start_y = glfw.get_cursor_pos(window)
            look_origin = start_x, start_y, viewport[2]
            io.add_mouse_pos_event(-1, -1)
            video_connection.enqueue(LOOK, action=glfw.PRESS)
            for key in MOVEMENT_KEYS:
                if glfw.get_key(window, key) == glfw.PRESS:
                    forwarded_keys.add(key)
                    video_connection.enqueue(KEY, key, glfw.PRESS, mods)
            return
        if button < 5:
            backend.mouse_button_callback(window, button, action, mods)
        if button > glfw.MOUSE_BUTTON_MIDDLE or not pos:
            return
        if action == glfw.PRESS and not io.want_capture_mouse:
            forwarded_buttons.add(button)
        elif action == glfw.RELEASE and button in forwarded_buttons:
            forwarded_buttons.remove(button)
        else:
            return
        video_connection.enqueue(BUTTON, button, action, mods, x=pos[0], y=pos[1])

    def on_scroll(_, dx, dy):
        nonlocal redraw
        redraw = True
        if look_origin is not None:
            return
        backend.scroll_callback(window, dx, dy)
        pos = position(*glfw.get_cursor_pos(window))
        if pos and not imgui.get_io().want_capture_mouse:
            video_connection.enqueue(MOVE, x=pos[0], y=pos[1])
            video_connection.enqueue(SCROLL, x=dx, y=dy)

    def on_focus(_, focused):
        nonlocal redraw
        redraw = True
        imgui.get_io().add_focus_event(bool(focused))
        if not focused:
            stop_look()
            forwarded_buttons.clear()
            forwarded_keys.clear()
            video_connection.enqueue(RELEASE_ALL)

    def on_refresh(*_):
        nonlocal redraw
        redraw = True

    glfw.set_key_callback(window, on_key)
    glfw.set_char_callback(window, backend.char_callback)
    glfw.set_cursor_pos_callback(window, cursor)
    glfw.set_mouse_button_callback(window, on_button)
    glfw.set_scroll_callback(window, on_scroll)
    glfw.set_window_focus_callback(window, on_focus)
    glfw.set_window_refresh_callback(window, on_refresh)
    glfw.set_framebuffer_size_callback(window, on_refresh)

    editor.refresh()
    rendered_at = 0.0
    try:
        while not glfw.window_should_close(window):
            glfw.poll_events()
            video_connection.send_inputs()
            control_connection.poll()
            now = time.monotonic()
            editor.poll_stats(now)
            for payload in video_connection.receive_frames():
                first_frame = video.width == 0
                if video.decode(payload):
                    if first_frame:
                        print(f"Receiving {video.width}x{video.height} H.264 video (NVDEC).", flush=True)
                    editor.received_frame(now)
                    redraw = True

            if redraw or editor.dirty or now - rendered_at >= 1.0 / 60.0:
                width, height = glfw.get_window_size(window)
                fb_width, fb_height = glfw.get_framebuffer_size(window)
                if not all((width, height, fb_width, fb_height)):
                    glfw.wait_events_timeout(0.01)
                    continue
                backend.process_inputs()
                imgui.new_frame()
                viewport = editor.draw(width, height, panel_width(width))
                imgui.render()

                GL.glDisable(GL.GL_SCISSOR_TEST)
                GL.glClearColor(0.045, 0.050, 0.059, 1.0)
                GL.glClear(GL.GL_COLOR_BUFFER_BIT)
                x, y, side, _ = viewport
                x0, x1 = (round(value * fb_width / width) for value in (x, x + side))
                y0, y1 = (round(value * fb_height / height) for value in (y, y + side))
                GL.glUseProgram(program)
                video.draw((x0, fb_height - y1, x1 - x0, y1 - y0))
                backend.render(imgui.get_draw_data())
                glfw.swap_buffers(window)
                rendered_at = now
                redraw = False
            else:
                glfw.wait_events_timeout(0.001)
    finally:
        editor.close()
        backend.shutdown()
        imgui.destroy_context()


def create_window():
    monitor = glfw.get_primary_monitor()
    workarea = glfw.get_monitor_workarea(monitor) if monitor else None
    glfw.window_hint(glfw.VISIBLE, glfw.FALSE)
    window = glfw.create_window(WINDOW_WIDTH, WINDOW_HEIGHT, "Falcor Remote Viewer", None, None)
    if not window:
        raise RuntimeError("Could not create the preview window")
    if workarea:
        x, y, work_width, work_height = workarea
        left, top, right, bottom = glfw.get_window_frame_size(window)
        width = max(1, min(WINDOW_WIDTH, work_width - left - right))
        height = max(1, min(WINDOW_HEIGHT, work_height - top - bottom))
        if (width, height) != (WINDOW_WIDTH, WINDOW_HEIGHT):
            glfw.set_window_size(window, width, height)
        glfw.set_window_pos(
            window,
            x + (work_width - width - left - right) // 2 + left,
            y + (work_height - height - top - bottom) // 2 + top,
        )
    glfw.show_window(window)
    return window


def connect(host, port, role, session):
    sock = socket.create_connection((host, port), timeout=5)
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.sendall(INPUT_EVENT.pack(role, session >> 32, session & 0xffffffff, 0, 0.0, 0.0))
        return sock
    except BaseException:
        sock.close()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    try:
        session = secrets.randbits(64) or 1
        with connect(args.host, args.port, OPEN_PREVIEW, session) as preview:
            with connect(args.host, args.port, OPEN_CONTROLS, session) as controls:
                if not glfw.init():
                    raise RuntimeError("Could not initialize GLFW")
                try:
                    glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 3)
                    glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 3)
                    glfw.window_hint(glfw.OPENGL_PROFILE, glfw.OPENGL_COMPAT_PROFILE)
                    window = create_window()
                    glfw.make_context_current(window)
                    glfw.swap_interval(0)
                    run(window, Connection(preview), ControlConnection(controls))
                finally:
                    glfw.terminate()
        return 0
    except Exception as error:
        print(f"FalcorRemoteViewer: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
