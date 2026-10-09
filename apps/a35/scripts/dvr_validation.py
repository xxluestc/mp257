"""MP4 validation and bounded final-video retention."""
import os
from pathlib import Path
import struct
import subprocess

MIN_MP4_BYTES = 4096
MAX_DVR_FILES = 12
MAX_DVR_BYTES = 2 * 1024 * 1024 * 1024

def run(command: list[str], log_path: Path) -> int:
    with log_path.open("wb") as log:
        return subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                              check=False, timeout=60).returncode


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
        return offset == size and {b"ftyp", b"mdat", b"moov"}.issubset(found)
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
        "ffmpeg", "-nostdin", "-v", "error", "-xerror", "-i", str(path), "-map", "0:v:0",
        "-f", "null", "-",
    ]
    if run(decode, decode_log) != 0:
        print("[DVR-WORKER] full decode reported errors", flush=True)
        return False
    return True


def prune_old_videos(directory: Path, current: Path) -> None:
    videos = sorted(
        (item for item in directory.glob("emergency_*.mp4") if item != current),
        key=lambda item: item.stat().st_mtime,
    )
    videos.append(current)
    total = sum(item.stat().st_size for item in videos)
    # Never delete the clip whose successful commit is being reported.
    while len(videos) > 1 and (len(videos) > MAX_DVR_FILES or total > MAX_DVR_BYTES):
        oldest = videos.pop(0)
        total -= oldest.stat().st_size
        oldest.unlink()
        print(f"[DVR-WORKER] capacity rotation removed {oldest.name}", flush=True)
