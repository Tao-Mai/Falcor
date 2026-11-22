"""Decode preview packets into host pixel planes without a display dependency."""

import ctypes
from dataclasses import dataclass
from enum import IntEnum
import io
import struct


PREVIEW_PROTOCOL = 1
PREVIEW_MAGIC = b"FCV1"
PREVIEW_HEADER = struct.Struct("!4sII")


class Codec(IntEnum):
    H264 = 1
    HEVC = 2
    HEVC_RGB = 3
    JPEG = 4


class PixelFormat(IntEnum):
    NV12 = 0
    GBR = 1
    RGB = 2


@dataclass(slots=True)
class Plane:
    stride: int
    pixels: bytes | ctypes.c_void_p


@dataclass(slots=True)
class DecodedFrame:
    width: int
    height: int
    format: PixelFormat
    planes: tuple[Plane, ...]
    owner: object = None


class VideoDecoder:
    def __init__(self, codec):
        import PyNvVideoCodec as nvc

        self.rgb = codec == Codec.HEVC_RGB
        self.native_format = nvc.Pixel_Format.YUV444 if self.rgb else nvc.Pixel_Format.NV12
        self.decoder = nvc.CreateDecoder(
            codec=nvc.cudaVideoCodec.H264 if codec == Codec.H264 else nvc.cudaVideoCodec.HEVC,
            usedevicememory=False, latency=nvc.DisplayDecodeLatencyType.ZERO,
        )
        self.packet = nvc.PacketData()
        self.packet.decode_flag = nvc.VideoPacketFlag.ENDOFPICTURE

    def decode(self, data, offset):
        self.packet.bsl = len(data) - offset
        self.packet.bsl_data = ctypes.addressof(ctypes.c_char.from_buffer(data, offset))
        frames = self.decoder.Decode(self.packet)
        if not frames:
            return None
        frame = frames[-1]
        if frame.format != self.native_format:
            raise ValueError(f"Unexpected NVDEC format {frame.format}")
        rows, width = frame.shape
        height = rows // 3 if self.rgb else rows * 2 // 3
        pixel_format = PixelFormat.GBR if self.rgb else PixelFormat.NV12
        planes = tuple(
            Plane(frame.strides[0], ctypes.c_void_p(frame.GetPtrToPlane(index)))
            for index in range(3 if self.rgb else 2)
        )
        return DecodedFrame(width, height, pixel_format, planes, frame)


def decode_jpeg(data, offset):
    from PIL import Image

    with Image.open(io.BytesIO(memoryview(data)[offset:]), formats=["JPEG"]) as image:
        width, height = image.size
        if max(width, height) > 4096:
            raise ValueError("JPEG preview is too large")
        pixels = image.convert("RGB").tobytes()
    return DecodedFrame(width, height, PixelFormat.RGB, (Plane(width * 3, pixels),))


class PreviewDecoder:
    DESCRIPTIONS = {
        Codec.H264: "H.264 video (NVDEC)",
        Codec.HEVC: "H.265 video (NVDEC)",
        Codec.HEVC_RGB: "HEVC RGB lossless video (NVDEC)",
        Codec.JPEG: "JPEG",
    }

    def __init__(self):
        self.stream = None
        self.decode_frame = None

    @property
    def description(self):
        return self.DESCRIPTIONS[self.stream[0]]

    def decode(self, data):
        if data.startswith(PREVIEW_MAGIC):
            if len(data) <= PREVIEW_HEADER.size:
                raise ValueError("Incomplete preview packet")
            _, codec, generation = PREVIEW_HEADER.unpack_from(data)
            stream = (Codec(codec), generation)
            offset = PREVIEW_HEADER.size
        else:
            # Older servers send bare H.264 access units.
            stream = (Codec.H264, 0)
            offset = 0
        try:
            if stream != self.stream:
                self.stream = stream
                self.decode_frame = None
                codec = stream[0]
                self.decode_frame = decode_jpeg if codec == Codec.JPEG else VideoDecoder(codec).decode
            if self.decode_frame is not None:
                return self.decode_frame(data, offset)
        except Exception:
            # Leave the last image on screen. Selecting another mode starts a fresh decoder.
            self.decode_frame = None
            raise
        return None
