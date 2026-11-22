"""Upload decoded pixel planes and draw the local preview."""

from OpenGL import GL
from OpenGL.GL import shaders

from preview_decoder import PixelFormat


FRAGMENT_SHADER = """#version 120
uniform sampler2D plane0;
uniform sampler2D plane1;
uniform sampler2D plane2;
uniform int pixel_format;
void main() {
    vec2 pos = gl_TexCoord[0].st;
    vec3 rgb;
    if (pixel_format == 2) {
        rgb = texture2D(plane0, pos).rgb;
    } else if (pixel_format == 1) {
        rgb = vec3(texture2D(plane2, pos).r, texture2D(plane0, pos).r, texture2D(plane1, pos).r);
    } else {
        float y = (texture2D(plane0, pos).r * 255.0 - 16.0) / 219.0;
        vec2 uv = (texture2D(plane1, pos).ra * 255.0 - 128.0) / 224.0;
        rgb = vec3(y + 1.5748 * uv.y,
                   y - 0.1873 * uv.x - 0.4681 * uv.y,
                   y + 1.8556 * uv.x);
    }
    gl_FragColor = vec4(rgb, 1.0);
}
"""


class PreviewDisplay:
    def __init__(self):
        self.width = self.height = 0
        self.format = None
        self.textures = tuple(int(texture) for texture in GL.glGenTextures(3))
        for texture in self.textures:
            GL.glBindTexture(GL.GL_TEXTURE_2D, texture)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MIN_FILTER, GL.GL_LINEAR)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_MAG_FILTER, GL.GL_LINEAR)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_S, GL.GL_CLAMP_TO_EDGE)
            GL.glTexParameteri(GL.GL_TEXTURE_2D, GL.GL_TEXTURE_WRAP_T, GL.GL_CLAMP_TO_EDGE)
        fragment = shaders.compileShader(FRAGMENT_SHADER, GL.GL_FRAGMENT_SHADER)
        self.program = shaders.compileProgram(fragment, validate=False)
        self.format_uniform = GL.glGetUniformLocation(self.program, "pixel_format")
        GL.glUseProgram(self.program)
        for index in range(len(self.textures)):
            GL.glUniform1i(GL.glGetUniformLocation(self.program, f"plane{index}"), index)

    def close(self):
        GL.glDeleteTextures(self.textures)
        GL.glDeleteProgram(self.program)

    def upload(self, frame):
        width, height = frame.width, frame.height
        resize = (width, height, frame.format) != (self.width, self.height, self.format)
        if frame.format == PixelFormat.NV12:
            layout = ((width, height, 1, GL.GL_LUMINANCE), (width // 2, height // 2, 2, GL.GL_LUMINANCE_ALPHA))
        elif frame.format == PixelFormat.GBR:
            layout = ((width, height, 1, GL.GL_LUMINANCE),) * 3
        else:
            layout = ((width, height, 3, GL.GL_RGB),)

        GL.glPixelStorei(GL.GL_UNPACK_ALIGNMENT, 1)
        try:
            for index, (plane, (w, h, channels, format_)) in enumerate(zip(frame.planes, layout)):
                GL.glActiveTexture(GL.GL_TEXTURE0 + index)
                GL.glBindTexture(GL.GL_TEXTURE_2D, self.textures[index])
                GL.glPixelStorei(GL.GL_UNPACK_ROW_LENGTH, plane.stride // channels)
                if resize:
                    GL.glTexImage2D(GL.GL_TEXTURE_2D, 0, format_, w, h, 0, format_, GL.GL_UNSIGNED_BYTE, plane.pixels)
                else:
                    GL.glTexSubImage2D(GL.GL_TEXTURE_2D, 0, 0, 0, w, h, format_, GL.GL_UNSIGNED_BYTE, plane.pixels)
        finally:
            GL.glPixelStorei(GL.GL_UNPACK_ROW_LENGTH, 0)
        self.width, self.height, self.format = width, height, frame.format

    def draw(self, rect):
        if not self.width or not rect[2] or not rect[3]:
            return
        GL.glUseProgram(self.program)
        GL.glUniform1i(self.format_uniform, int(self.format))
        for index, texture in enumerate(self.textures):
            GL.glActiveTexture(GL.GL_TEXTURE0 + index)
            GL.glBindTexture(GL.GL_TEXTURE_2D, texture)
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
