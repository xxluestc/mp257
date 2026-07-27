#!/usr/bin/env python3
"""Host-side integration test for the four OTA APIs and release switching."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Optional

from helmet_ota_common import OTAError, read_json, validate_package
from helmet_ota_installer import Installer
from helmet_ota_server import OTAApplication, OTAServer


HERE = Path(__file__).resolve().parent
NO_PROXY_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def request_json(
    base_url: str,
    path: str,
    *,
    method: str = "GET",
    body: Optional[bytes] = None,
    headers: Optional[dict[str, str]] = None,
) -> tuple[int, dict[str, object]]:
    request = urllib.request.Request(
        base_url + path,
        data=body,
        method=method,
        headers=headers or {},
    )
    try:
        with NO_PROXY_OPENER.open(request, timeout=10) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        content = error.read()
        try:
            result = json.loads(content)
        except (ValueError, json.JSONDecodeError):
            result = {"ok": False, "error": content.decode("utf-8", "replace")}
        return error.code, result


def upload(base_url: str, package: Path) -> str:
    content = package.read_bytes()
    digest = hashlib.sha256(content).hexdigest()
    status, result = request_json(
        base_url,
        "/api/ota/upload",
        method="POST",
        body=content,
        headers={
            "Content-Type": "application/gzip",
            "X-OTA-SHA256": digest,
        },
    )
    if status != 201 or result.get("ok") is not True:
        raise AssertionError(f"upload failed: HTTP {status} {result}")
    package_id = result.get("package_id")
    if not isinstance(package_id, str):
        raise AssertionError(f"upload returned invalid package_id: {result}")
    return package_id


def install_and_wait(base_url: str, package_id: str) -> dict[str, object]:
    body = json.dumps({"package_id": package_id}).encode("utf-8")
    status, accepted = request_json(
        base_url,
        "/api/ota/install",
        method="POST",
        body=body,
        headers={"Content-Type": "application/json"},
    )
    if status != 202 or accepted.get("accepted") is not True:
        raise AssertionError(f"install was not accepted: HTTP {status} {accepted}")

    deadline = time.monotonic() + 30
    latest: dict[str, object] = {}
    while time.monotonic() < deadline:
        status, latest = request_json(base_url, "/api/ota/status")
        if status != 200:
            raise AssertionError(f"status request failed: HTTP {status} {latest}")
        if latest.get("state") in {"success", "failed", "rolled_back"}:
            break
        time.sleep(0.1)
    if latest.get("state") != "success":
        raise AssertionError(f"installation did not succeed: {latest}")
    return latest


def assert_package_scope(package: Path, version: str) -> None:
    info = validate_package(package)
    if info.version != version:
        raise AssertionError(f"{package} contains VERSION={info.version}")
    with tarfile.open(package, "r:gz") as archive:
        files = {item.name for item in archive.getmembers() if item.isfile()}
    forbidden = {
        "radar_config",
        "radar_fusion.cpp",
        "main.c",
        "dvr_system.log",
        "project_CM33_NonSecure.elf",
    }
    unexpected = files & forbidden
    if unexpected:
        raise AssertionError(f"runtime package contains forbidden files: {unexpected}")


def runtime_file_digests(package: Path) -> dict[str, str]:
    digests: dict[str, str] = {}
    with tarfile.open(package, "r:gz") as archive:
        for item in archive.getmembers():
            if not item.isfile() or item.name in {"VERSION", "MANIFEST.sha256"}:
                continue
            source = archive.extractfile(item)
            if source is None:
                raise AssertionError(f"cannot read {item.name}")
            with source:
                digests[item.name] = hashlib.sha256(source.read()).hexdigest()
    return digests


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=HERE.parent / "dist")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    dist = args.dist.resolve()
    packages = {
        version: dist / f"helmet-a35-{version}.tar.gz"
        for version in ("1.0.0", "1.0.1")
    }
    for version, package in packages.items():
        if not package.is_file():
            raise SystemExit(
                f"missing {package}; build VERSION=1.0.0 and VERSION=1.0.1 first"
            )
        assert_package_scope(package, version)
    if runtime_file_digests(packages["1.0.0"]) != runtime_file_digests(
        packages["1.0.1"]
    ):
        raise AssertionError("the two test packages unexpectedly differ in runtime files")

    with tempfile.TemporaryDirectory(prefix="helmet-ota-test-") as temporary:
        root = Path(temporary)
        xxl_root = root / "xxl"
        legacy = xxl_root / "camera_detect"
        legacy.mkdir(parents=True)
        site_config = b"TTC=3.7\nDIST=1.8\nDASHBOARD_PORT=8080\n"
        (legacy / "radar_config").write_bytes(site_config)
        state_dir = root / "state"

        server_args = argparse.Namespace(
            state_dir=state_dir,
            xxl_root=xxl_root,
            installer=HERE / "helmet_ota_installer.py",
            max_upload_bytes=256 * 1024 * 1024,
            health_timeout=3,
            test_mode=True,
        )
        app = OTAApplication(server_args)
        server = OTAServer(("127.0.0.1", 0), app)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        host, port = server.server_address
        base_url = f"http://{host}:{port}"

        try:
            status, version = request_json(base_url, "/api/ota/version")
            if status != 200 or version.get("current_version") != "legacy":
                raise AssertionError(f"unexpected initial version: {version}")

            status, rejected = request_json(
                base_url,
                "/api/ota/upload",
                method="POST",
                body=b"corrupt-package",
                headers={
                    "Content-Type": "application/gzip",
                    "X-OTA-SHA256": "0" * 64,
                },
            )
            if status != 400 or rejected.get("ok") is not False:
                raise AssertionError(f"bad package was not rejected: {rejected}")

            package_id = upload(base_url, packages["1.0.0"])
            result = install_and_wait(base_url, package_id)
            if result.get("current_version") != "1.0.0":
                raise AssertionError(f"wrong first installed version: {result}")
            if not (xxl_root / "camera_detect").is_symlink():
                raise AssertionError("legacy application directory was not migrated")
            if (xxl_root / "camera_detect/radar_config").read_bytes() != site_config:
                raise AssertionError("site radar_config was not inherited")

            package_id = upload(base_url, packages["1.0.1"])
            result = install_and_wait(base_url, package_id)
            if result.get("current_version") != "1.0.1":
                raise AssertionError(f"wrong second installed version: {result}")
            if (xxl_root / "camera_detect/radar_config").read_bytes() != site_config:
                raise AssertionError("site radar_config changed after second OTA")

            rollback = subprocess.run(
                [
                    sys.executable,
                    str(HERE / "helmet_ota_installer.py"),
                    "--rollback",
                    "--state-dir",
                    str(state_dir),
                    "--xxl-root",
                    str(xxl_root),
                    "--health-timeout",
                    "3",
                    "--test-mode",
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            if rollback.returncode != 0:
                raise AssertionError(
                    f"manual rollback failed: {rollback.stdout}{rollback.stderr}"
                )
            status, version = request_json(base_url, "/api/ota/version")
            if status != 200 or version.get("current_version") != "1.0.0":
                raise AssertionError(f"rollback selected wrong version: {version}")

            # Exercise the installer's automatic rollback branch. The package
            # is valid and switching succeeds, but the injected health failure
            # represents a board application that cannot become healthy.
            shutil.rmtree(xxl_root / "releases/1.0.1")
            installer_args = argparse.Namespace(
                state_dir=state_dir,
                xxl_root=xxl_root,
                service="dvr.service",
                health_timeout=3,
                test_mode=True,
            )
            installer = Installer(installer_args)
            original_wait_for_health = installer.wait_for_health

            def fail_new_release(expected_version: Optional[str]) -> str:
                if expected_version == "1.0.1":
                    raise OTAError("injected new-release health failure")
                return original_wait_for_health(expected_version)

            installer.wait_for_health = fail_new_release  # type: ignore[method-assign]
            info = validate_package(packages["1.0.1"])
            installer.install(
                packages["1.0.1"],
                info.sha256,
                "automatic-rollback-test",
                "automaticrollback",
            )
            final_status = read_json(state_dir / "status.json")
            if (
                final_status.get("state") != "rolled_back"
                or final_status.get("current_version") != "1.0.0"
                or final_status.get("rollback_performed") is not True
            ):
                raise AssertionError(f"automatic rollback failed: {final_status}")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    print("OTA host integration test passed:")
    print("  version -> upload -> install -> status")
    print("  legacy migration and radar_config inheritance")
    print("  1.0.0 -> 1.0.1 -> manual rollback to 1.0.0")
    print("  injected health failure -> automatic rollback to 1.0.0")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
