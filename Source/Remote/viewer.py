"""Show the remote Falcor viewport using NVDEC and forward mouse and keyboard input."""

import argparse
from collections import deque
import ctypes
import socket
import struct
import sys

import glfw
from OpenGL import GL
from OpenGL.GL import shaders
import PyNvVideoCodec as nvc


FRAME_SIZE = struct.Struct("!I")
INPUT_EVENT = struct.Struct("!4I2f")
KEY, MOVE, BUTTON, SCROLL, RELEASE_ALL = range(1, 6)
MAX_FRAME_BYTES = 16 * 1024 * 1024

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
        can_replace = len(self.inputs) > 1 or (self.inputs and self.input_offset == 0)
        if kind == MOVE and can_replace and self.inputs[-1][0] == MOVE:
            self.inputs[-1] = (kind, data)
        elif len(self.inputs) < 256:
            self.inputs.append((kind, data))
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
                raise ConnectionError("Mogwai closed the input connection")
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
                        raise ValueError("Invalid H.264 packet size from Mogwai")
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
                    raise ConnectionError("Mogwai closed the video connection")
                self.buffer.extend(data)
                read_socket = True
            yield payload


def fit_video(width, height, video_width, video_height):
    scale = min(width / video_width, height / video_height)
    w = max(1, int(video_width * scale))
    h = max(1, int(video_height * scale))
    return (width - w) // 2, (height - h) // 2, w, h


class Display:
    def __init__(self):
        self.width = self.height = 0
        fragment = shaders.compileShader(FRAGMENT_SHADER, GL.GL_FRAGMENT_SHADER)
        program = shaders.compileProgram(fragment, validate=False)
        GL.glUseProgram(program)
        GL.glUniform1i(GL.glGetUniformLocation(program, "luma"), 0)
        GL.glUniform1i(GL.glGetUniformLocation(program, "chroma"), 1)
        self.textures = GL.glGenTextures(2)
        for texture in self.textures:
            GL.glBindTexture(GL.GL_TEXTURE_2D, int(texture))
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_LINEAR)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_LINEAR)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_S, GL.GL_CLAMP_TO_EDGE)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_T, GL.GL_CLAMP_TO_EDGE)
        GL.glPixelStorei(GL.GL_UNPACK_ALIGNMENT, 1)

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

    def draw(self, window):
        width, height = glfw.get_framebuffer_size(window)
        GL.glClear(GL.GL_COLOR_BUFFER_BIT)
        if self.width and width and height:
            left, top, w, h = fit_video(width, height, self.width, self.height)
            GL.glViewport(left, height - top - h, w, h)
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
        glfw.swap_buffers(window)


def run(window, connection):
    display = Display()
    decoder = nvc.CreateDecoder(
        codec=nvc.cudaVideoCodec.H264, usedevicememory=False,
        latency=nvc.DisplayDecodeLatencyType.ZERO,
    )
    packet = nvc.PacketData()
    packet.decode_flag = nvc.VideoPacketFlag.ENDOFPICTURE
    redraw = True

    def cursor(x, y, kind=MOVE, code=0, action=0, mods=0):
        width, height = glfw.get_window_size(window)
        if not width or not height:
            return
        left, top, w, h = (0, 0, width, height)
        if display.width:
            left, top, w, h = fit_video(width, height, display.width, display.height)
        connection.enqueue(kind, code, action, mods, (x - left) / w, (y - top) / h)

    def on_key(_, key, scan_code, action, mods):
        if key != glfw.KEY_UNKNOWN:
            connection.enqueue(KEY, key, action, mods)

    def on_button(_, button, action, mods):
        x, y = glfw.get_cursor_pos(window)
        cursor(x, y, BUTTON, button, action, mods)

    def on_refresh(*_):
        nonlocal redraw
        redraw = True

    glfw.set_key_callback(window, on_key)
    glfw.set_cursor_pos_callback(window, lambda _, x, y: cursor(x, y))
    glfw.set_mouse_button_callback(window, on_button)
    glfw.set_scroll_callback(window, lambda _, x, y: connection.enqueue(SCROLL, x=x, y=y))
    glfw.set_window_focus_callback(window, lambda _, focused: None if focused else connection.enqueue(RELEASE_ALL))
    glfw.set_window_refresh_callback(window, on_refresh)
    glfw.set_framebuffer_size_callback(window, on_refresh)

    while not glfw.window_should_close(window):
        glfw.poll_events()
        if glfw.window_should_close(window):
            break
        connection.send_inputs()
        frame = None
        for data in connection.receive_frames():
            packet.bsl = len(data)
            packet.bsl_data = ctypes.addressof(ctypes.c_char.from_buffer(data))
            frames = decoder.Decode(packet)
            if frames:
                frame = frames[-1]
        if frame is not None:
            first_frame = display.width == 0
            display.upload(frame)
            if first_frame:
                print(f"Receiving {display.width}x{display.height} H.264 video (NVDEC).", flush=True)
            redraw = True
        if redraw:
            display.draw(window)
            redraw = False
        else:
            glfw.wait_events_timeout(0.002)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("port must be between 1 and 65535")
    try:
        with socket.create_connection((args.host, args.port), timeout=5) as sock:
            if not glfw.init():
                raise RuntimeError("Could not initialize GLFW")
            try:
                glfw.window_hint(glfw.CONTEXT_VERSION_MAJOR, 2)
                glfw.window_hint(glfw.CONTEXT_VERSION_MINOR, 1)
                window = glfw.create_window(1280, 720, "Falcor Remote Viewer", None, None)
                if not window:
                    raise RuntimeError("Could not create the preview window")
                glfw.make_context_current(window)
                glfw.swap_interval(0)
                run(window, Connection(sock))
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
