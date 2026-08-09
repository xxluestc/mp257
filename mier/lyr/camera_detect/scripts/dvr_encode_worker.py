#!/usr/bin/env python3
"""Encode one closed DVR MJPEG buffer in a fresh process.

radar_fusion is multi-threaded.  The worker is exec'ed with posix_spawn so no
malloc/stdio/GStreamer work runs in a post-fork copy of that process.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

MIN_MP4_BYTES = 4096
MAX_DVR_FILES = 12
MAX_DVR_BYTES = 2 * 1024 * 1024 * 1024


def load_job(path: Path) -> tuple[Path, Path, float, list[tuple[int, int]]]:
    lines = path.read_text(encoding="ascii").splitlines()
    if not lines or lines[0] != "DVRJOB1":
        raise ValueError("invalid DVR job header")
    values: dict[str, str] = {}
    entries: list[tuple[int, int]] = []
    reading_entries = False
    for line in lines[1:]:
        if line == "entries":
            reading_entries = True
            continue
        if reading_entries:
            offset, size = line.split("\t", 1)
            entries.append((int(offset), int(size)))
        else:
            key, value = line.split("\t", 1)
            values[key] = value
    if len(entries) < 2:
        raise ValueError("DVR job has fewer than two frames")
    return (
        Path(values["raw"]),
        Path(values["output"]),
        float(values["fps"]),
        entries,
    )


def run(command: list[str], log_path: Path) -> int:
    with log_path.open("wb") as log:
        return subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                              check=False).returncode


def has_mp4_boxes(path: Path) -> bool:
    try:
        size = path.stat().st_size
        if size < MIN_MP4_BYTES:
            return False
        found: set[bytes] = set()
        with path.open("rb") as stream:
            offset = 0
            while offset + 8 <= size:
                stream.seek(offset)
                header = stream.read(16)
                if len(header) < 8:
                    return False
                box_size, box_type = struct.unpack(">I4s", header[:8])
                header_size = 8
                if box_size == 1:
                    if len(header) < 16:
                        return False
                    box_size = struct.unpack(">Q", header[8:16])[0]
                    header_size = 16
                elif box_size == 0:
                    box_size = size - offset
                if box_size < header_size or box_size > size - offset:
                    return False
                found.add(box_type)
                offset += box_size
        return {b"ftyp", b"mdat", b"moov"}.issubset(found)
    except OSError:
        return False


def sync_file(path: Path) -> None:
    with path.open("r+b", buffering=0) as stream:
        os.fsync(stream.fileno())


def validate(path: Path, probe_log: Path, decode_log: Path) -> bool:
    try:
        sync_file(path)
    except OSError as exc:
        print(f"[DVR-WORKER] fsync failed: {exc}", flush=True)
        return False
    if not has_mp4_boxes(path):
        print("[DVR-WORKER] missing/invalid ftyp+mdat+moov", flush=True)
        return False
    probe = [
        "ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=codec_name,width,height,duration",
        "-of", "csv=p=0", str(path),
    ]
    if run(probe, probe_log) != 0:
        print("[DVR-WORKER] ffprobe rejected video", flush=True)
        return False
    decode = [
        "ffmpeg", "-nostdin", "-v", "error", "-i", str(path), "-map", "0:v:0",
        "-f", "null", "-",
    ]
    if run(decode, decode_log) != 0:
        print("[DVR-WORKER] full decode reported errors", flush=True)
        return False
    return True


def copy_to_storage(source: Path, destination: Path) -> None:
    with source.open("rb") as src, destination.open("wb") as dst:
        shutil.copyfileobj(src, dst, length=1024 * 1024)
        dst.flush()
        os.fsync(dst.fileno())


def prune_old_videos(directory: Path, current: Path) -> None:
    videos = sorted(
        (item for item in directory.glob("emergency_*.mp4") if item != current),
        key=lambda item: item.stat().st_mtime,
    )
    videos.append(current)
    total = sum(item.stat().st_size for item in videos)
    while len(videos) > MAX_DVR_FILES or total > MAX_DVR_BYTES:
        oldest = videos.pop(0)
        total -= oldest.stat().st_size
        oldest.unlink()
        print(f"[DVR-WORKER] capacity rotation removed {oldest.name}", flush=True)


def extract_frames(raw_path: Path, work_dir: Path,
                   entries: list[tuple[int, int]]) -> None:
    work_dir.mkdir(mode=0o777)
    with raw_path.open("rb") as raw:
        for index, (offset, expected_size) in enumerate(entries):
            raw.seek(offset)
            header = raw.read(12)
            if len(header) != 12:
                raise ValueError(f"short raw header at frame {index}")
            frame_size = struct.unpack("=I", header[:4])[0]
            jpeg_size = frame_size - 12
            if frame_size < 16 or jpeg_size != expected_size or jpeg_size > 16 * 1024 * 1024:
                raise ValueError(f"invalid raw header at frame {index}")
            jpeg = raw.read(jpeg_size)
            if len(jpeg) != jpeg_size or not jpeg.startswith(b"\xff\xd8"):
                raise ValueError(f"invalid JPEG at frame {index}")
            frame_path = work_dir / f"frame_{index:06d}.jpg"
            with frame_path.open("wb") as frame:
                frame.write(jpeg)


def encode(work_dir: Path, fps: float, local_part: Path,
           encoder_log: Path, probe_log: Path, decode_log: Path) -> str | None:
    pattern = str(work_dir / "frame_%06d.jpg")
    gst_fps = max(1, round(fps))
    commands = [
        (
            "gst-launch MP4 H.264",
            [
                "gst-launch-1.0", "-e", "multifilesrc", f"location={pattern}",
                "start-index=0", f"caps=image/jpeg,framerate={gst_fps}/1",
                "!", "jpegdec", "!", "videoconvert", "!",
                "video/x-raw,format=NV12", "!", "v4l2slh264enc",
                "bitrate=4000000", "!", "h264parse", "!", "mp4mux", "!",
                "filesink", f"location={local_part}",
            ],
        ),
        (
            "ffmpeg MPEG-4",
            [
                "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "warning",
                "-framerate", f"{fps:.2f}", "-i", pattern, "-c:v", "mpeg4",
                "-q:v", "5", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
                "-f", "mp4", "-y", str(local_part),
            ],
        ),
    ]
    for name, command in commands:
        local_part.unlink(missing_ok=True)
        rc = run(command, encoder_log)
        if rc == 0 and validate(local_part, probe_log, decode_log):
            return name
        print(f"[DVR-WORKER] {name} failed validation (rc={rc})", flush=True)
    return None


def process(job_path: Path) -> int:
    buffer_dir = job_path.parent
    work_dir = buffer_dir / ".encode"
    encoder_log = buffer_dir / "dvr_encoder.log"
    probe_log = buffer_dir / "dvr_ffprobe.log"
    decode_log = buffer_dir / "dvr_decode.log"
    raw_path, output_path, fps, entries = load_job(job_path)
    local_part = Path(f"/tmp/helmet_dvr_{os.getpid()}.mp4.part")
    storage_part = Path(f"{output_path}.part")

    try:
        extract_frames(raw_path, work_dir, entries)
        print(f"[DVR-WORKER] extracted {len(entries)} frames at {fps:.2f} fps",
              flush=True)
        encoder_name = encode(work_dir, fps, local_part, encoder_log,
                              probe_log, decode_log)
        if encoder_name is None:
            raise RuntimeError("all encoders failed validation")

        storage_part.unlink(missing_ok=True)
        copy_to_storage(local_part, storage_part)
        if not validate(storage_part, probe_log, decode_log):
            raise RuntimeError("storage copy failed second validation")

        os.replace(storage_part, output_path)
        directory_fd = os.open(output_path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
        prune_old_videos(output_path.parent, output_path)

        local_part.unlink(missing_ok=True)
        shutil.rmtree(work_dir)
        encoder_log.unlink(missing_ok=True)
        probe_log.unlink(missing_ok=True)
        decode_log.unlink(missing_ok=True)
        job_path.unlink(missing_ok=True)
        raw_path.unlink(missing_ok=True)
        buffer_dir.rmdir()
        print(f"[DVR-WORKER] VALIDATED {output_path} ({encoder_name})", flush=True)
        return 0
    except Exception as exc:  # preserve raw/JPEG/logs for diagnosis
        local_part.unlink(missing_ok=True)
        storage_part.unlink(missing_ok=True)
        failed_dir = Path(f"{buffer_dir}.failed_{os.getpid()}")
        try:
            os.replace(buffer_dir, failed_dir)
            location = failed_dir
        except OSError:
            location = buffer_dir
        print(f"[DVR-WORKER] FAILED: {exc}; recovery={location}", flush=True)
        return 2


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--job", required=True, type=Path)
    args = parser.parse_args()
    return process(args.job)


if __name__ == "__main__":
    sys.exit(main())
