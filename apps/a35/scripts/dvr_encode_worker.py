#!/usr/bin/env python3
"""Consume timestamped JPEG records from RAM; commit only complete validated MP4s.

Wire format: repeated little-endian <uint64 monotonic_us, uint32 jpeg_bytes>
and JPEG payload. A twelve-byte zero record marks successful end-of-event.
EOF without that marker is an interrupted recording and cannot be committed.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from typing import BinaryIO, Iterator

from dvr_validation import prune_old_videos, validate

MAX_JPEG_BYTES = 1024 * 1024
MAX_FRAMES = 2500
MAX_STREAM_US = 95 * 1000000
HEADER = struct.Struct("<QI")


def read_exact(stream: BinaryIO, size: int) -> bytes:
    result = bytearray()
    while len(result) < size:
        block = stream.read(size - len(result))
        if not block:
            raise ValueError("interrupted DVR stream")
        result.extend(block)
    return bytes(result)


def records(stream: BinaryIO) -> Iterator[tuple[int, bytes]]:
    first = previous = count = 0
    while True:
        timestamp, size = HEADER.unpack(read_exact(stream, HEADER.size))
        if size == 0:
            if timestamp != 0 or count < 2:
                raise ValueError("invalid end marker or insufficient frames")
            return
        if size < 4 or size > MAX_JPEG_BYTES or timestamp <= previous or count >= MAX_FRAMES:
            raise ValueError("invalid DVR record")
        if not first:
            first = timestamp
        if timestamp - first > MAX_STREAM_US:
            raise ValueError("DVR event exceeds duration limit")
        jpeg = read_exact(stream, size)
        # UVC MJPEG may contain padding after EOI. The encoder and full-video
        # decode validate the payload; do not reject valid padded camera frames.
        if not jpeg.startswith(b"\xff\xd8"):
            raise ValueError("invalid JPEG boundaries")
        previous = timestamp
        count += 1
        yield timestamp, jpeg


def hardware_gst():
    try:
        import gi
        gi.require_version("Gst", "1.0")
        from gi.repository import Gst
        Gst.init(None)
        needed = ("appsrc", "jpegdec", "videoconvert", "v4l2slh264enc", "h264parse", "mp4mux")
        if all(Gst.ElementFactory.find(name) is not None for name in needed):
            return Gst
    except (ImportError, ValueError):
        pass
    return None


def encode_gstreamer(stream: BinaryIO, part: Path, Gst) -> None:
    # No path interpolation into a pipeline expression: filesink.location is a property.
    pipeline = Gst.parse_launch(
        "appsrc name=input format=time is-live=false block=true max-bytes=1048576 "
        "caps=image/jpeg,framerate=25/1 ! jpegdec ! videoconvert ! "
        "video/x-raw,format=NV12 ! v4l2slh264enc bitrate=4000000 ! h264parse ! "
        "mp4mux ! filesink name=output"
    )
    pipeline.get_by_name("output").set_property("location", str(part))
    source = pipeline.get_by_name("input")
    bus = pipeline.get_bus()
    first = None
    try:
        if pipeline.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
            raise RuntimeError("GStreamer PLAYING failed")
        for timestamp, jpeg in records(stream):
            if first is None:
                first = timestamp
            error = bus.pop_filtered(Gst.MessageType.ERROR)
            if error is not None:
                raise RuntimeError(str(error.parse_error()))
            buffer = Gst.Buffer.new_allocate(None, len(jpeg), None)
            buffer.fill(0, jpeg)
            buffer.pts = (timestamp - first) * 1000
            buffer.dts = Gst.CLOCK_TIME_NONE
            buffer.duration = 40 * Gst.MSECOND
            if source.emit("push-buffer", buffer) != Gst.FlowReturn.OK:
                raise RuntimeError("GStreamer rejected JPEG")
        if source.emit("end-of-stream") != Gst.FlowReturn.OK:
            raise RuntimeError("GStreamer EOS failed")
        message = bus.timed_pop_filtered(30 * Gst.SECOND,
                                        Gst.MessageType.EOS | Gst.MessageType.ERROR)
        if message is None or message.type != Gst.MessageType.EOS:
            raise RuntimeError("GStreamer mux did not finish successfully")
    finally:
        pipeline.set_state(Gst.State.NULL)


def encode_ffmpeg(stream: BinaryIO, part: Path, log: Path) -> None:
    # The compatibility backend uses fixed 25fps. It cannot represent gaps with
    # original PTS; report that choice explicitly instead of claiming H.264 hardware encoding.
    command = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning",
               "-f", "image2pipe", "-framerate", "25", "-vcodec", "mjpeg", "-i", "pipe:0",
               "-c:v", "mpeg4", "-q:v", "5", "-pix_fmt", "yuv420p",
               "-movflags", "+faststart", "-f", "mp4", "-y", str(part)]
    with log.open("wb") as output:
        child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=output, stderr=output)
        try:
            for _, jpeg in records(stream):
                child.stdin.write(jpeg)
            child.stdin.close()
            if child.wait(timeout=60) != 0:
                raise RuntimeError("FFmpeg encoder failed")
        finally:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)


def require_storage(mount: Path, output: Path) -> None:
    if mount.is_symlink() or not mount.is_dir() or mount.stat().st_dev == mount.parent.stat().st_dev:
        raise RuntimeError("TF mount is unavailable")
    resolved = output.parent.resolve()
    resolved.relative_to(mount.resolve())
    output.parent.mkdir(parents=True, exist_ok=True)


def process(stream: BinaryIO, output: Path, mount: Path, backend: str) -> int:
    part = Path(f"{output}.part")
    encoder_log = Path(f"{output}.encoder.log")
    probe_log = Path(f"{output}.probe.log")
    decode_log = Path(f"{output}.decode.log")
    owns_part = False
    try:
        require_storage(mount, output)
        if output.exists() or output.is_symlink():
            raise RuntimeError("DVR output already exists")
        # Reserve the temporary path exclusively. A rejected invocation must
        # never unlink another encoder's existing .part file.
        with part.open("xb"):
            pass
        owns_part = True
        if shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None:
            raise RuntimeError("ffmpeg/ffprobe validation dependency unavailable")
        Gst = hardware_gst() if backend != "ffmpeg" else None
        if backend == "gstreamer" and Gst is None:
            raise RuntimeError("GStreamer hardware backend unavailable")
        print(f"[DVR-WORKER] backend={'gstreamer_h264' if Gst else 'ffmpeg_mpeg4'}", flush=True)
        if Gst:
            encode_gstreamer(stream, part, Gst)
        else:
            encode_ffmpeg(stream, part, encoder_log)
        if not validate(part, probe_log, decode_log):
            raise RuntimeError("MP4 validation failed")
        require_storage(mount, output)
        os.replace(part, output)
        owns_part = False
        directory = os.open(output.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        # Retention maintenance cannot turn an already committed clip into an
        # encoding failure; its errors have a separate diagnostic.
        try:
            prune_old_videos(output.parent, output)
            for log in (encoder_log, probe_log, decode_log):
                log.unlink(missing_ok=True)
        except OSError as exc:
            print(f"[DVR-WORKER] maintenance failed: {exc}", flush=True)
        print(f"[DVR-WORKER] VALIDATED {output}", flush=True)
        return 0
    except Exception as exc:
        print(f"[DVR-WORKER] FAILED: {exc}", flush=True)
        if owns_part:
            try:
                part.unlink(missing_ok=True)
            except OSError as cleanup:
                print(f"[DVR-WORKER] partial cleanup failed: {cleanup}", flush=True)
        # Cleanup is best effort even when the TF disappears during encoding.
        try:
            logs = sorted(output.parent.glob("emergency_*.mp4.*.log"),
                          key=lambda item: item.stat().st_mtime)
            for old in logs[:-36]:
                old.unlink(missing_ok=True)
        except OSError as cleanup:
            print(f"[DVR-WORKER] diagnostic cleanup failed: {cleanup}", flush=True)
        return 2


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stream", required=True, action="store_true")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--mount", required=True, type=Path)
    parser.add_argument("--backend", choices=("auto", "gstreamer", "ffmpeg"), default="auto")
    args = parser.parse_args()
    return process(sys.stdin.buffer, args.output, args.mount, args.backend)


if __name__ == "__main__":
    sys.exit(main())
