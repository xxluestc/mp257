#!/usr/bin/env python3
"""Shared validation and state helpers for the helmet A35 OTA service."""

from __future__ import annotations

import hashlib
import json
import os
import re
import tarfile
import tempfile
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any, BinaryIO, Optional


VERSION_RE = re.compile(r"^[0-9][0-9A-Za-z._-]{0,63}$")
PACKAGE_ID_RE = re.compile(r"^[0-9a-f]{16,64}-[0-9a-f]{8}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
MAX_ARCHIVE_MEMBERS = 4096
MAX_UNCOMPRESSED_BYTES = 1024 * 1024 * 1024
MAX_MANIFEST_BYTES = 4 * 1024 * 1024

ALLOWED_TOP_LEVEL = {
    "VERSION",
    "MANIFEST.sha256",
    "radar_fusion",
    "hud",
    "start_dvr.sh",
    "dvr.service.example",
    "dvr-m33.service.example",
    "radar_config.example",
    "models",
    "sounds",
    "dashboard",
    "scripts",
    "nav_tts_cache",
    "stai_mpu",
}

REQUIRED_FILES = {
    "VERSION",
    "MANIFEST.sha256",
    "radar_fusion",
    "hud",
    "start_dvr.sh",
    "dvr.service.example",
    "dvr-m33.service.example",
    "radar_config.example",
    "models/ssd_mobilenet_v2_fpnlite_10_256_int8_per_tensor.nb",
    "models/labels_coco_dataset_80.txt",
    "dashboard/radar_dashboard.py",
    "dashboard/static/index.html",
    "dashboard/static/styles.css",
    "dashboard/static/app.js",
    "scripts/log_maintenance.sh",
}

ALLOWED_DIRECTORIES = {
    "models",
    "sounds",
    "dashboard",
    "dashboard/static",
    "scripts",
    "nav_tts_cache",
    "stai_mpu",
}

ALLOWED_FIXED_FILES = REQUIRED_FILES | {
    "dvr.service.example",
    "dvr-m33.service.example",
}


class OTAError(RuntimeError):
    """User-visible OTA validation or installation error."""


@dataclass(frozen=True)
class PackageInfo:
    version: str
    sha256: str
    compressed_bytes: int
    uncompressed_bytes: int
    file_count: int


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def validate_version(version: str) -> str:
    version = version.strip()
    if not VERSION_RE.fullmatch(version):
        raise OTAError(
            "invalid VERSION; use 1-64 characters: digits, letters, '.', '_' or '-'"
        )
    return version


def sha256_file(path: Path, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(chunk_size):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_stream(source: BinaryIO, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    while chunk := source.read(chunk_size):
        digest.update(chunk)
    return digest.hexdigest()


def atomic_write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(value, output, ensure_ascii=False, sort_keys=True, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def read_json(
    path: Path, default: Optional[dict[str, Any]] = None
) -> dict[str, Any]:
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
        return loaded if isinstance(loaded, dict) else dict(default or {})
    except (FileNotFoundError, OSError, ValueError, json.JSONDecodeError):
        return dict(default or {})


def current_version(app_path: Path) -> str:
    try:
        return validate_version((app_path / "VERSION").read_text(encoding="utf-8"))
    except (FileNotFoundError, OSError, OTAError):
        return "legacy" if app_path.exists() else "unknown"


def _safe_member_name(name: str) -> str:
    if not name or "\\" in name or name.startswith("/"):
        raise OTAError(f"unsafe archive path: {name!r}")
    path = PurePosixPath(name)
    if any(part in ("", ".", "..") for part in path.parts):
        raise OTAError(f"unsafe archive path: {name!r}")
    if path.parts[0] not in ALLOWED_TOP_LEVEL:
        raise OTAError(f"unexpected top-level OTA path: {path.parts[0]}")
    if path.name == "radar_config":
        raise OTAError("OTA package must not contain the site radar_config")
    return path.as_posix()


def _allowed_runtime_member(name: str, is_directory: bool) -> bool:
    if is_directory:
        return name in ALLOWED_DIRECTORIES
    if name in ALLOWED_FIXED_FILES:
        return True

    path = PurePosixPath(name)
    if len(path.parts) != 2:
        return False
    directory, filename = path.parts
    if directory in {"sounds", "nav_tts_cache"}:
        return filename.endswith(".wav")
    if directory == "scripts":
        return filename.endswith(".sh")
    if directory == "stai_mpu":
        return ".so" in filename and filename.startswith("lib")
    return False


def _read_small_member(
    archive: tarfile.TarFile, member: tarfile.TarInfo, maximum: int
) -> bytes:
    if member.size > maximum:
        raise OTAError(f"{member.name} exceeds size limit")
    source = archive.extractfile(member)
    if source is None:
        raise OTAError(f"cannot read archive member: {member.name}")
    with source:
        return source.read(maximum + 1)


def _parse_manifest(content: str) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line_number, line in enumerate(content.splitlines(), start=1):
        if not line:
            continue
        if len(line) < 67 or line[64:66] != "  ":
            raise OTAError(f"invalid MANIFEST.sha256 line {line_number}")
        digest = line[:64].lower()
        name = _safe_member_name(line[66:])
        if not SHA256_RE.fullmatch(digest):
            raise OTAError(f"invalid SHA-256 on manifest line {line_number}")
        if name == "MANIFEST.sha256" or name in entries:
            raise OTAError(f"invalid or duplicate manifest entry: {name}")
        entries[name] = digest
    return entries


def validate_package(
    package_path: Path, expected_sha256: Optional[str] = None
) -> PackageInfo:
    package_path = package_path.resolve()
    if not package_path.is_file():
        raise OTAError(f"OTA package not found: {package_path}")

    actual_package_sha = sha256_file(package_path)
    if expected_sha256 is not None:
        expected_sha256 = expected_sha256.strip().lower()
        if not SHA256_RE.fullmatch(expected_sha256):
            raise OTAError("expected package SHA-256 is invalid")
        if actual_package_sha != expected_sha256:
            raise OTAError("uploaded package SHA-256 mismatch")

    try:
        archive = tarfile.open(package_path, mode="r:gz")
    except (tarfile.TarError, OSError) as error:
        raise OTAError(f"invalid tar.gz package: {error}") from error

    with archive:
        members = archive.getmembers()
        if not members or len(members) > MAX_ARCHIVE_MEMBERS:
            raise OTAError("invalid OTA archive member count")

        member_by_name: dict[str, tarfile.TarInfo] = {}
        total_size = 0
        regular_files: set[str] = set()
        for member in members:
            name = _safe_member_name(member.name)
            if name in member_by_name:
                raise OTAError(f"duplicate archive member: {name}")
            if not (member.isdir() or member.isreg()):
                raise OTAError(f"links/devices are forbidden in OTA packages: {name}")
            if not _allowed_runtime_member(name, member.isdir()):
                raise OTAError(f"file is outside the OTA runtime allowlist: {name}")
            member_by_name[name] = member
            if member.isreg():
                regular_files.add(name)
                total_size += member.size
                if total_size > MAX_UNCOMPRESSED_BYTES:
                    raise OTAError("OTA package exceeds uncompressed size limit")

        missing = REQUIRED_FILES - regular_files
        if missing:
            raise OTAError(f"OTA package is missing: {', '.join(sorted(missing))}")

        version_bytes = _read_small_member(
            archive, member_by_name["VERSION"], maximum=128
        )
        try:
            version = validate_version(version_bytes.decode("utf-8"))
        except UnicodeDecodeError as error:
            raise OTAError("VERSION must be UTF-8") from error

        manifest_bytes = _read_small_member(
            archive,
            member_by_name["MANIFEST.sha256"],
            maximum=MAX_MANIFEST_BYTES,
        )
        try:
            manifest = _parse_manifest(manifest_bytes.decode("utf-8"))
        except UnicodeDecodeError as error:
            raise OTAError("MANIFEST.sha256 must be UTF-8") from error

        expected_manifest_files = regular_files - {"MANIFEST.sha256"}
        if set(manifest) != expected_manifest_files:
            missing_manifest = expected_manifest_files - set(manifest)
            extra_manifest = set(manifest) - expected_manifest_files
            details = []
            if missing_manifest:
                details.append(f"missing={sorted(missing_manifest)}")
            if extra_manifest:
                details.append(f"extra={sorted(extra_manifest)}")
            raise OTAError("manifest file set mismatch: " + " ".join(details))

        for name in sorted(expected_manifest_files):
            source = archive.extractfile(member_by_name[name])
            if source is None:
                raise OTAError(f"cannot hash archive member: {name}")
            with source:
                actual = sha256_stream(source)
            if actual != manifest[name]:
                raise OTAError(f"manifest SHA-256 mismatch: {name}")

    return PackageInfo(
        version=version,
        sha256=actual_package_sha,
        compressed_bytes=package_path.stat().st_size,
        uncompressed_bytes=total_size,
        file_count=len(regular_files),
    )


def extract_validated_package(
    package_path: Path,
    destination: Path,
    expected_sha256: Optional[str] = None,
) -> PackageInfo:
    info = validate_package(package_path, expected_sha256)
    destination.mkdir(parents=True, exist_ok=False)

    try:
        with tarfile.open(package_path, mode="r:gz") as archive:
            for member in archive.getmembers():
                name = _safe_member_name(member.name)
                target = destination.joinpath(*PurePosixPath(name).parts)
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                    os.chmod(target, member.mode & 0o755 or 0o755)
                    continue

                target.parent.mkdir(parents=True, exist_ok=True)
                source = archive.extractfile(member)
                if source is None:
                    raise OTAError(f"cannot extract archive member: {name}")
                with source, target.open("xb") as output:
                    while chunk := source.read(1024 * 1024):
                        output.write(chunk)
                    output.flush()
                    os.fsync(output.fileno())
                os.chmod(target, member.mode & 0o755 or 0o644)
    except Exception:
        import shutil

        shutil.rmtree(destination, ignore_errors=True)
        raise

    return info


def package_info_dict(info: PackageInfo) -> dict[str, Any]:
    return asdict(info)
