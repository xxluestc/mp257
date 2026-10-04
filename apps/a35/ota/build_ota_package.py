#!/usr/bin/env python3
"""Build a deterministic A35 runtime-only OTA tarball."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import os
import shutil
import sys
import tarfile
import tempfile
from pathlib import Path

from helmet_ota_common import OTAError, validate_package, validate_version


ROOT = Path(__file__).resolve().parent.parent


def copy_file(source: Path, destination: Path, executable: bool = False) -> None:
    if not source.is_file():
        raise FileNotFoundError(f"required OTA input is missing: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    os.chmod(destination, 0o755 if executable else 0o644)


def copy_matching(
    source_dir: Path,
    destination_dir: Path,
    pattern: str,
    executable: bool = False,
) -> None:
    matches = sorted(source_dir.glob(pattern))
    if not matches:
        raise FileNotFoundError(f"no OTA inputs matched: {source_dir / pattern}")
    for source in matches:
        if source.is_file():
            copy_file(source, destination_dir / source.name, executable)


def populate_payload(payload: Path, version: str) -> None:
    (payload / "VERSION").write_text(version + "\n", encoding="utf-8")
    os.chmod(payload / "VERSION", 0o644)

    copy_file(ROOT / "radar_fusion", payload / "radar_fusion", executable=True)
    copy_file(ROOT / "hud/hud", payload / "hud", executable=True)
    copy_file(ROOT / "start_dvr.sh", payload / "start_dvr.sh", executable=True)
    copy_file(ROOT / "dvr.service.example", payload / "dvr.service.example")
    copy_file(
        ROOT / "dvr-m33.service.example", payload / "dvr-m33.service.example"
    )
    copy_file(ROOT / "radar_config.example", payload / "radar_config.example")

    copy_file(
        ROOT / "models/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb",
        payload / "models/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb",
    )
    copy_file(
        ROOT / "models/labels_coco_dataset_80.txt",
        payload / "models/labels_coco_dataset_80.txt",
    )

    copy_matching(ROOT / "sounds", payload / "sounds", "*.wav")
    copy_matching(ROOT / "nav_tts_cache", payload / "nav_tts_cache", "*.wav")
    copy_matching(ROOT / "scripts", payload / "scripts", "*.sh", executable=True)
    copy_matching(ROOT / "scripts", payload / "scripts", "*.py", executable=True)
    copy_matching(ROOT / "stai_mpu", payload / "stai_mpu", "*.so*")

    copy_file(
        ROOT / "dashboard/radar_dashboard.py",
        payload / "dashboard/radar_dashboard.py",
        executable=True,
    )
    for name in ("index.html", "styles.css", "app.js"):
        copy_file(
            ROOT / f"dashboard/static/{name}",
            payload / f"dashboard/static/{name}",
        )


def write_manifest(payload: Path) -> None:
    lines: list[str] = []
    for path in sorted(item for item in payload.rglob("*") if item.is_file()):
        if path.name == "MANIFEST.sha256":
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        lines.append(f"{digest}  {path.relative_to(payload).as_posix()}\n")
    (payload / "MANIFEST.sha256").write_text("".join(lines), encoding="utf-8")
    os.chmod(payload / "MANIFEST.sha256", 0o644)


def normalized_tarinfo(archive_name: str, path: Path) -> tarfile.TarInfo:
    info = tarfile.TarInfo(archive_name)
    stat = path.stat()
    info.size = stat.st_size if path.is_file() else 0
    info.mode = stat.st_mode & 0o777
    info.mtime = 0
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    info.type = tarfile.REGTYPE if path.is_file() else tarfile.DIRTYPE
    return info


def create_tarball(payload: Path, output: Path) -> None:
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.unlink(missing_ok=True)
    try:
        with temporary.open("wb") as raw_output:
            with gzip.GzipFile(
                filename="", mode="wb", fileobj=raw_output, mtime=0, compresslevel=9
            ) as gzip_output:
                with tarfile.open(fileobj=gzip_output, mode="w") as archive:
                    for path in sorted(payload.rglob("*")):
                        relative = path.relative_to(payload).as_posix()
                        info = normalized_tarinfo(relative, path)
                        if path.is_file():
                            with path.open("rb") as source:
                                archive.addfile(info, source)
                        else:
                            archive.addfile(info)
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--dist", type=Path, default=ROOT / "dist")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    version = validate_version(args.version)
    dist = args.dist.resolve()
    dist.mkdir(parents=True, exist_ok=True)
    output = dist / f"helmet-a35-{version}.tar.gz"

    with tempfile.TemporaryDirectory(prefix="helmet-a35-package-") as temporary:
        payload = Path(temporary) / "payload"
        payload.mkdir()
        populate_payload(payload, version)
        write_manifest(payload)
        create_tarball(payload, output)

    info = validate_package(output)
    sha_path = output.with_name(output.name + ".sha256")
    sha_path.write_text(f"{info.sha256}  {output.name}\n", encoding="ascii")
    print(f"OTA package: {output}")
    print(f"SHA-256:    {sha_path}")
    print(
        f"VERSION={info.version} files={info.file_count} "
        f"compressed={info.compressed_bytes} bytes"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OTAError, FileNotFoundError, OSError) as error:
        print(f"OTA package error: {error}", file=sys.stderr)
        raise SystemExit(1)
