#!/usr/bin/env python3
"""Independent HTTP service for uploading and installing helmet A35 OTA."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import threading
import uuid
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

from helmet_ota_common import (
    OTAError,
    PACKAGE_ID_RE,
    SHA256_RE,
    atomic_write_json,
    current_version,
    package_info_dict,
    read_json,
    utc_now,
    validate_package,
)


API_VERSION = 1
DEFAULT_STATE_DIR = Path("/var/lib/helmet-ota")
DEFAULT_XXL_ROOT = Path("/xxl")
DEFAULT_MAX_UPLOAD_BYTES = 256 * 1024 * 1024


class OTAApplication:
    def __init__(self, args: argparse.Namespace) -> None:
        self.state_dir = args.state_dir.resolve()
        self.upload_dir = self.state_dir / "uploads"
        self.status_path = self.state_dir / "status.json"
        self.xxl_root = args.xxl_root.resolve()
        self.app_path = self.xxl_root / "camera_detect"
        self.installer = Path(args.installer).resolve()
        self.max_upload_bytes = args.max_upload_bytes
        self.health_timeout = args.health_timeout
        self.test_mode = args.test_mode
        self.lock = threading.Lock()
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.upload_dir.mkdir(parents=True, exist_ok=True)
        if not self.status_path.exists():
            atomic_write_json(
                self.status_path,
                {
                    "state": "idle",
                    "phase": "idle",
                    "progress": 0,
                    "updated_at": utc_now(),
                },
            )

    def status(self) -> dict[str, Any]:
        value = read_json(
            self.status_path,
            {"state": "idle", "phase": "idle", "progress": 0},
        )
        value["current_version"] = current_version(self.app_path)
        value["api_version"] = API_VERSION
        return value

    def version(self) -> dict[str, Any]:
        last_success = read_json(self.state_dir / "last_success.json")
        target = None
        if self.app_path.is_symlink():
            target = os.path.realpath(self.app_path)
        elif self.app_path.exists():
            target = str(self.app_path)
        return {
            "ok": True,
            "api_version": API_VERSION,
            "current_version": current_version(self.app_path),
            "current_target": target,
            "previous_version": last_success.get("previous_version"),
            "previous_target": last_success.get("previous_target"),
        }

    def receive_upload(
        self,
        source: Any,
        content_length: int,
        expected_sha256: str,
    ) -> dict[str, Any]:
        expected_sha256 = expected_sha256.strip().lower()
        if not SHA256_RE.fullmatch(expected_sha256):
            raise OTAError("X-OTA-SHA256 must contain 64 lowercase hex characters")
        if content_length <= 0:
            raise OTAError("Content-Length must be greater than zero")
        if content_length > self.max_upload_bytes:
            raise OTAError(
                f"OTA upload exceeds {self.max_upload_bytes} byte limit"
            )

        temporary = self.upload_dir / f".upload-{uuid.uuid4().hex}.tmp"
        digest = hashlib.sha256()
        remaining = content_length
        try:
            with temporary.open("xb") as output:
                while remaining:
                    chunk = source.read(min(1024 * 1024, remaining))
                    if not chunk:
                        raise OTAError("request body ended before Content-Length")
                    output.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
                output.flush()
                os.fsync(output.fileno())

            actual_sha256 = digest.hexdigest()
            if actual_sha256 != expected_sha256:
                raise OTAError("uploaded package SHA-256 mismatch")

            info = validate_package(temporary, expected_sha256)
            package_id = f"{actual_sha256[:16]}-{uuid.uuid4().hex[:8]}"
            package_path = self.upload_dir / f"{package_id}.tar.gz"
            os.replace(temporary, package_path)
            metadata = {
                "package_id": package_id,
                "package_path": str(package_path),
                "uploaded_at": utc_now(),
                **package_info_dict(info),
            }
            atomic_write_json(
                self.upload_dir / f"{package_id}.json",
                metadata,
            )
            with self.lock:
                status = self.status()
                if status.get("state") != "installing":
                    atomic_write_json(
                        self.status_path,
                        {
                            "state": "uploaded",
                            "phase": "ready",
                            "progress": 0,
                            "package_id": package_id,
                            "target_version": info.version,
                            "package_sha256": info.sha256,
                            "current_version": current_version(self.app_path),
                            "updated_at": utc_now(),
                        },
                    )
            return {"ok": True, **metadata}
        finally:
            temporary.unlink(missing_ok=True)

    def _rotate_install_log(self) -> Path:
        log_path = self.state_dir / "install.log"
        try:
            if log_path.stat().st_size >= 5 * 1024 * 1024:
                backup = self.state_dir / "install.log.1"
                backup.unlink(missing_ok=True)
                os.replace(log_path, backup)
        except FileNotFoundError:
            pass
        return log_path

    def start_install(self, package_id: str) -> dict[str, Any]:
        if not PACKAGE_ID_RE.fullmatch(package_id):
            raise OTAError("invalid package_id")
        metadata_path = self.upload_dir / f"{package_id}.json"
        metadata = read_json(metadata_path)
        if metadata.get("package_id") != package_id:
            raise OTAError("uploaded package metadata not found")
        package_path = Path(str(metadata.get("package_path", "")))
        if not package_path.is_file() or package_path.parent != self.upload_dir:
            raise OTAError("uploaded package file not found")

        with self.lock:
            status = self.status()
            if status.get("state") == "installing":
                raise OTAError("another OTA installation is already running")

            job_id = uuid.uuid4().hex
            queued = {
                "state": "installing",
                "phase": "queued",
                "progress": 1,
                "job_id": job_id,
                "package_id": package_id,
                "target_version": metadata.get("version"),
                "package_sha256": metadata.get("sha256"),
                "current_version": current_version(self.app_path),
                "started_at": utc_now(),
                "updated_at": utc_now(),
                "error": None,
                "rollback_performed": False,
            }
            atomic_write_json(self.status_path, queued)

            command = [
                sys.executable,
                str(self.installer),
                "--install",
                "--package",
                str(package_path),
                "--sha256",
                str(metadata["sha256"]),
                "--package-id",
                package_id,
                "--job-id",
                job_id,
                "--state-dir",
                str(self.state_dir),
                "--xxl-root",
                str(self.xxl_root),
                "--health-timeout",
                str(self.health_timeout),
            ]
            if self.test_mode:
                command.append("--test-mode")

            try:
                log_path = self._rotate_install_log()
                log_output = log_path.open("ab", buffering=0)
                process = subprocess.Popen(
                    command,
                    stdin=subprocess.DEVNULL,
                    stdout=log_output,
                    stderr=subprocess.STDOUT,
                    close_fds=True,
                    start_new_session=True,
                )
                log_output.close()
            except (OSError, KeyError) as error:
                queued.update(
                    {
                        "state": "failed",
                        "phase": "launch",
                        "progress": 100,
                        "error": f"cannot launch OTA installer: {error}",
                        "finished_at": utc_now(),
                        "updated_at": utc_now(),
                    }
                )
                atomic_write_json(self.status_path, queued)
                raise OTAError(queued["error"]) from error

            # Do not rewrite status.json here: the child can reach its
            # "validating" state before Popen returns and that state must not
            # be overwritten by the older queued snapshot.
            threading.Thread(target=process.wait, daemon=True).start()

        return {
            "ok": True,
            "accepted": True,
            "job_id": job_id,
            "package_id": package_id,
            "target_version": metadata.get("version"),
            "installer_pid": process.pid,
        }


class OTARequestHandler(BaseHTTPRequestHandler):
    server_version = "HelmetOTA/1"

    @property
    def app(self) -> OTAApplication:
        return self.server.app  # type: ignore[attr-defined]

    def send_json(self, status: HTTPStatus, value: dict[str, Any]) -> None:
        body = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode(
            "utf-8"
        )
        self.send_response(status.value)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def error_json(self, status: HTTPStatus, message: str) -> None:
        self.send_json(status, {"ok": False, "error": message})

    def do_GET(self) -> None:
        path = urlparse(self.path).path
        if path == "/api/ota/version":
            self.send_json(HTTPStatus.OK, self.app.version())
        elif path == "/api/ota/status":
            self.send_json(HTTPStatus.OK, {"ok": True, **self.app.status()})
        else:
            self.error_json(HTTPStatus.NOT_FOUND, "endpoint not found")

    def do_POST(self) -> None:
        path = urlparse(self.path).path
        try:
            if path == "/api/ota/upload":
                length_header = self.headers.get("Content-Length")
                if length_header is None:
                    self.error_json(
                        HTTPStatus.LENGTH_REQUIRED, "Content-Length is required"
                    )
                    return
                try:
                    content_length = int(length_header)
                except ValueError:
                    self.error_json(
                        HTTPStatus.BAD_REQUEST, "Content-Length is invalid"
                    )
                    return
                expected_sha = self.headers.get("X-OTA-SHA256", "")
                result = self.app.receive_upload(
                    self.rfile, content_length, expected_sha
                )
                self.send_json(HTTPStatus.CREATED, result)
                return

            if path == "/api/ota/install":
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > 65536:
                    self.error_json(
                        HTTPStatus.BAD_REQUEST, "small JSON request body is required"
                    )
                    return
                try:
                    request = json.loads(self.rfile.read(length))
                except (ValueError, json.JSONDecodeError):
                    self.error_json(HTTPStatus.BAD_REQUEST, "invalid JSON body")
                    return
                if not isinstance(request, dict):
                    self.error_json(HTTPStatus.BAD_REQUEST, "JSON object is required")
                    return
                package_id = request.get("package_id")
                if not isinstance(package_id, str):
                    self.error_json(HTTPStatus.BAD_REQUEST, "package_id is required")
                    return
                result = self.app.start_install(package_id)
                self.send_json(HTTPStatus.ACCEPTED, result)
                return

            self.error_json(HTTPStatus.NOT_FOUND, "endpoint not found")
        except OTAError as error:
            status = (
                HTTPStatus.CONFLICT
                if "already running" in str(error)
                else HTTPStatus.BAD_REQUEST
            )
            self.error_json(status, str(error))
        except Exception as error:
            self.log_error("unhandled OTA error: %s", error)
            self.error_json(HTTPStatus.INTERNAL_SERVER_ERROR, "internal OTA error")

    def log_message(self, format_string: str, *args: Any) -> None:
        super().log_message(format_string, *args)


class OTAServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self, address: tuple[str, int], app: OTAApplication
    ) -> None:
        super().__init__(address, OTARequestHandler)
        self.app = app


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8090)
    parser.add_argument("--state-dir", type=Path, default=DEFAULT_STATE_DIR)
    parser.add_argument("--xxl-root", type=Path, default=DEFAULT_XXL_ROOT)
    parser.add_argument(
        "--installer",
        type=Path,
        default=Path(__file__).resolve().parent / "helmet_ota_installer.py",
    )
    parser.add_argument(
        "--max-upload-bytes", type=int, default=DEFAULT_MAX_UPLOAD_BYTES
    )
    parser.add_argument("--health-timeout", type=int, default=45)
    parser.add_argument("--test-mode", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    app = OTAApplication(args)
    server = OTAServer((args.host, args.port), app)
    print(
        f"[HELMET_OTA] listening on http://{args.host}:{args.port} "
        f"state_dir={app.state_dir}"
    )
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
