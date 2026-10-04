#!/usr/bin/env python3
"""Transactional installer and rollback worker for helmet A35 OTA releases."""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request
import uuid
from pathlib import Path
from typing import Any, Optional

from helmet_ota_common import (
    OTAError,
    atomic_write_json,
    current_version,
    extract_validated_package,
    read_json,
    utc_now,
    validate_package,
)


DEFAULT_STATE_DIR = Path("/var/lib/helmet-ota")
DEFAULT_XXL_ROOT = Path("/xxl")


class Installer:
    def __init__(self, args: argparse.Namespace) -> None:
        self.state_dir = args.state_dir.resolve()
        self.xxl_root = args.xxl_root.resolve()
        self.releases_dir = self.xxl_root / "releases"
        self.app_path = self.xxl_root / "camera_detect"
        self.persistent_dir = self.xxl_root / "persistent/camera_detect"
        self.status_path = self.state_dir / "status.json"
        self.last_success_path = self.state_dir / "last_success.json"
        self.lock_path = self.state_dir / "install.lock"
        self.service = args.service
        self.dashboard_service = getattr(
            args, "dashboard_service", "radar-dashboard.service"
        )
        self.health_timeout = args.health_timeout
        self.test_mode = args.test_mode
        self.lock_file: Any = None

    def acquire_lock(self) -> None:
        self.state_dir.mkdir(parents=True, exist_ok=True)
        self.lock_file = self.lock_path.open("a+")
        try:
            fcntl.flock(self.lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise OTAError("another OTA installation is already running") from error

    def status(self, **updates: Any) -> dict[str, Any]:
        value = read_json(self.status_path)
        value.update(updates)
        value["updated_at"] = utc_now()
        atomic_write_json(self.status_path, value)
        return value

    def run_systemctl_unit(
        self, action: str, unit: str, check: bool = True
    ) -> bool:
        if self.test_mode:
            return True
        result = subprocess.run(
            ["systemctl", action, unit],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        if check and result.returncode != 0:
            raise OTAError(
                f"systemctl {action} {unit} failed: {result.stderr.strip()}"
            )
        return result.returncode == 0

    def run_systemctl(self, action: str, check: bool = True) -> bool:
        return self.run_systemctl_unit(action, self.service, check)

    def stop_application(self) -> None:
        self.run_systemctl("stop")

    def start_application(self) -> None:
        self.run_systemctl("start")
        # Dashboard is intentionally independent from dvr.service, but its
        # Python backend lives inside the switched release. Restart it after
        # every switch/rollback so the backend and static assets use one version.
        self.run_systemctl_unit(
            "restart", self.dashboard_service, check=False
        )

    def dashboard_port(self) -> int:
        config = self.app_path / "radar_config"
        try:
            for line in config.read_text(encoding="utf-8").splitlines():
                if line.startswith("DASHBOARD_PORT="):
                    port = int(line.split("=", 1)[1])
                    if 1 <= port <= 65535:
                        return port
        except (FileNotFoundError, OSError, ValueError):
            pass
        return 8080

    def _process_running(self, *command: str) -> bool:
        result = subprocess.run(
            list(command),
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return result.returncode == 0

    def health_once(self, expected_version: Optional[str]) -> tuple[bool, str]:
        if expected_version is not None:
            actual_version = current_version(self.app_path)
            if actual_version != expected_version:
                return False, f"VERSION is {actual_version}, expected {expected_version}"

        if not (self.app_path / "radar_fusion").is_file():
            return False, "radar_fusion is missing from active release"
        if not (self.app_path / "hud").is_file() and not self.test_mode:
            return False, "HUD binary is missing from active release"

        if self.test_mode:
            return True, "test-mode release structure is healthy"

        if not self._process_running("systemctl", "is-active", "--quiet", self.service):
            return False, f"{self.service} is not active"
        if not self._process_running("pgrep", "-x", "radar_fusion"):
            return False, "radar_fusion process is not running"
        if not self._process_running("pgrep", "-x", "hud"):
            return False, "HUD process is not running"

        try:
            with urllib.request.urlopen(
                f"http://127.0.0.1:{self.dashboard_port()}/api/state", timeout=2
            ) as response:
                state = json.load(response)
            if response.status != 200:
                return False, f"Dashboard returned HTTP {response.status}"
            if not isinstance(state, dict) or "stale" not in state:
                return False, "Dashboard returned an invalid state payload"
        except (
            OSError,
            ValueError,
            json.JSONDecodeError,
            urllib.error.URLError,
        ) as error:
            return False, f"Dashboard health request failed: {error}"

        return True, "dvr, radar_fusion, HUD and Dashboard are healthy"

    def wait_for_health(self, expected_version: Optional[str]) -> str:
        deadline = time.monotonic() + self.health_timeout
        last_reason = "health check not started"
        while time.monotonic() < deadline:
            healthy, last_reason = self.health_once(expected_version)
            if healthy:
                return last_reason
            time.sleep(1)
        raise OTAError(f"health check timed out: {last_reason}")

    def active_target(self) -> Optional[Path]:
        if self.app_path.is_symlink():
            return Path(os.path.realpath(self.app_path))
        if self.app_path.is_dir():
            return self.app_path
        return None

    def switch_symlink(self, target: Path) -> None:
        if not target.is_dir():
            raise OTAError(f"release target does not exist: {target}")
        temporary = self.xxl_root / f".camera_detect.ota-{uuid.uuid4().hex}"
        temporary.unlink(missing_ok=True)
        os.symlink(target, temporary)
        try:
            if self.app_path.is_dir() and not self.app_path.is_symlink():
                raise OTAError("cannot replace legacy directory without migration")
            os.replace(temporary, self.app_path)
        finally:
            temporary.unlink(missing_ok=True)

    def prepare_site_config(self, release_dir: Path) -> None:
        self.persistent_dir.mkdir(parents=True, exist_ok=True)
        persistent_config = self.persistent_dir / "radar_config"
        if not persistent_config.exists():
            current_config = self.app_path / "radar_config"
            if current_config.is_file():
                shutil.copy2(current_config, persistent_config)
            else:
                shutil.copy2(release_dir / "radar_config.example", persistent_config)
            os.chmod(persistent_config, 0o644)

        release_config = release_dir / "radar_config"
        if release_config.exists() or release_config.is_symlink():
            raise OTAError("release unexpectedly contains radar_config")
        os.symlink(persistent_config, release_config)

    def set_previous_link(self, target: Optional[Path]) -> None:
        previous_link = self.releases_dir / ".previous"
        temporary = self.releases_dir / f".previous.tmp-{uuid.uuid4().hex}"
        temporary.unlink(missing_ok=True)
        if target is None:
            previous_link.unlink(missing_ok=True)
            return
        os.symlink(target, temporary)
        os.replace(temporary, previous_link)

    def migrate_legacy_directory(self, legacy_target: Path) -> None:
        if not self.app_path.is_dir() or self.app_path.is_symlink():
            raise OTAError("legacy application directory disappeared during migration")
        os.rename(self.app_path, legacy_target)

    def rollback_to(
        self,
        previous_target: Path,
        previous_version: str,
        failed_target: Optional[Path],
        reason: str,
    ) -> None:
        self.status(
            state="installing",
            phase="rolling_back",
            error=reason,
            rollback_performed=True,
        )
        self.run_systemctl("stop", check=False)
        self.switch_symlink(previous_target)
        self.start_application()
        expected = previous_version if previous_version not in ("legacy", "unknown") else None
        health = self.wait_for_health(expected)
        self.status(
            state="rolled_back",
            phase="complete",
            progress=100,
            current_version=current_version(self.app_path),
            current_target=str(previous_target),
            failed_target=str(failed_target) if failed_target else None,
            error=reason,
            health=health,
            rollback_performed=True,
            finished_at=utc_now(),
        )

    def install(
        self,
        package: Path,
        expected_sha256: str,
        package_id: str,
        job_id: str,
    ) -> None:
        if not self.test_mode and os.geteuid() != 0:
            raise OTAError("OTA installer must run as root")

        self.acquire_lock()
        switched = False
        service_stopped = False
        previous_target: Optional[Path] = None
        previous_version = current_version(self.app_path)
        target_dir: Optional[Path] = None
        staging: Optional[Path] = None

        self.status(
            state="installing",
            phase="validating",
            progress=5,
            package_id=package_id,
            job_id=job_id,
            from_version=previous_version,
            rollback_performed=False,
            error=None,
            started_at=utc_now(),
        )

        try:
            info = validate_package(package, expected_sha256)
            if info.version == previous_version:
                raise OTAError(
                    f"VERSION {info.version} is already active; use a new VERSION"
                )

            self.releases_dir.mkdir(parents=True, exist_ok=True)
            target_dir = self.releases_dir / info.version
            if target_dir.exists() or target_dir.is_symlink():
                raise OTAError(f"release already exists: {target_dir}")

            staging = self.releases_dir / (
                f".staging-{info.version}-{job_id.replace('-', '')}"
            )
            if staging.exists():
                shutil.rmtree(staging)

            self.status(
                phase="extracting",
                progress=20,
                target_version=info.version,
                package_sha256=info.sha256,
                staging_dir=str(staging),
            )
            extract_validated_package(package, staging, expected_sha256)
            self.prepare_site_config(staging)
            os.rename(staging, target_dir)
            staging = None
            self.status(
                phase="preparing_switch",
                progress=35,
                target_dir=str(target_dir),
            )

            previous_target = self.active_target()
            legacy_target: Optional[Path] = None
            if previous_target == self.app_path:
                timestamp = time.strftime("%Y%m%d-%H%M%S")
                legacy_target = self.releases_dir / f"legacy-{timestamp}"
                if legacy_target.exists():
                    raise OTAError(f"legacy backup path already exists: {legacy_target}")
                previous_target = legacy_target

            self.status(
                phase="stopping_application",
                progress=45,
                previous_target=str(previous_target) if previous_target else None,
                previous_version=previous_version,
                target_dir=str(target_dir),
            )
            self.stop_application()
            service_stopped = True

            if legacy_target is not None:
                self.status(phase="migrating_legacy_directory", progress=55)
                self.migrate_legacy_directory(legacy_target)

            self.status(phase="switching_release", progress=65)
            self.switch_symlink(target_dir)
            switched = True

            self.status(phase="starting_application", progress=75)
            self.start_application()
            service_stopped = False

            self.status(phase="health_check", progress=85)
            health = self.wait_for_health(info.version)

            self.set_previous_link(previous_target)
            success = {
                "current_version": info.version,
                "current_target": str(target_dir),
                "previous_version": previous_version,
                "previous_target": str(previous_target) if previous_target else None,
                "package_id": package_id,
                "package_sha256": info.sha256,
                "installed_at": utc_now(),
            }
            atomic_write_json(self.last_success_path, success)
            self.status(
                state="success",
                phase="complete",
                progress=100,
                current_version=info.version,
                current_target=str(target_dir),
                previous_version=previous_version,
                previous_target=str(previous_target) if previous_target else None,
                health=health,
                rollback_performed=False,
                error=None,
                finished_at=utc_now(),
            )
        except Exception as error:
            message = str(error) or error.__class__.__name__
            if staging is not None:
                shutil.rmtree(staging, ignore_errors=True)

            if switched and previous_target is not None and previous_target.is_dir():
                try:
                    self.rollback_to(
                        previous_target, previous_version, target_dir, message
                    )
                    return
                except Exception as rollback_error:
                    message = f"{message}; rollback failed: {rollback_error}"
            elif service_stopped:
                try:
                    self.start_application()
                except Exception as start_error:
                    message = f"{message}; restart failed: {start_error}"

            self.status(
                state="failed",
                phase="complete",
                progress=100,
                error=message,
                rollback_performed=False,
                finished_at=utc_now(),
            )
            raise OTAError(message) from error

    def manual_rollback(self) -> None:
        if not self.test_mode and os.geteuid() != 0:
            raise OTAError("OTA rollback must run as root")
        self.acquire_lock()
        record = read_json(self.last_success_path)
        previous_raw = record.get("previous_target")
        if not previous_raw:
            raise OTAError("no previous stable release is recorded")
        previous_target = Path(previous_raw)
        if not previous_target.is_dir():
            raise OTAError(f"previous release is missing: {previous_target}")

        old_current_target = self.active_target()
        old_current_version = current_version(self.app_path)
        previous_version = str(record.get("previous_version") or "legacy")
        self.status(
            state="installing",
            phase="manual_rollback",
            progress=20,
            from_version=old_current_version,
            target_version=previous_version,
            previous_target=str(previous_target),
            started_at=utc_now(),
        )
        self.stop_application()
        self.switch_symlink(previous_target)
        self.start_application()
        expected = previous_version if previous_version not in ("legacy", "unknown") else None
        health = self.wait_for_health(expected)

        new_record = {
            "current_version": current_version(self.app_path),
            "current_target": str(previous_target),
            "previous_version": old_current_version,
            "previous_target": (
                str(old_current_target) if old_current_target is not None else None
            ),
            "installed_at": utc_now(),
            "manual_rollback": True,
        }
        atomic_write_json(self.last_success_path, new_record)
        self.set_previous_link(old_current_target)
        self.status(
            state="success",
            phase="manual_rollback_complete",
            progress=100,
            current_version=current_version(self.app_path),
            current_target=str(previous_target),
            previous_version=old_current_version,
            previous_target=(
                str(old_current_target) if old_current_target is not None else None
            ),
            health=health,
            rollback_performed=True,
            error=None,
            finished_at=utc_now(),
        )

    def recover(self) -> None:
        self.acquire_lock()
        status = read_json(self.status_path)
        if status.get("state") != "installing":
            return

        previous_raw = status.get("previous_target")
        previous_version = str(status.get("previous_version") or "legacy")
        if not previous_raw:
            active_target = self.active_target()
            target_version = str(status.get("target_version") or "")
            for field in ("staging_dir", "target_dir"):
                candidate_raw = status.get(field)
                if not candidate_raw:
                    continue
                candidate = Path(str(candidate_raw)).resolve()
                try:
                    relative = candidate.relative_to(self.releases_dir.resolve())
                except ValueError:
                    continue
                if len(relative.parts) != 1:
                    continue
                expected_name = (
                    candidate.name.startswith(".staging-")
                    if field == "staging_dir"
                    else bool(target_version) and candidate.name == target_version
                )
                if candidate != active_target and expected_name:
                    shutil.rmtree(candidate, ignore_errors=True)
            # The interruption happened before the application was stopped.
            # Make a best effort to leave the existing application available.
            self.run_systemctl("start", check=False)
            self.status(
                state="failed",
                phase="recovery",
                error="interrupted OTA occurred before release switching",
                finished_at=utc_now(),
            )
            return

        previous_target = Path(previous_raw)
        if not previous_target.is_dir():
            # During legacy migration previous_target is recorded before the
            # ordinary /xxl/camera_detect directory is renamed. If the rename
            # never happened, the original directory is still the valid app.
            if (
                previous_version == "legacy"
                and self.app_path.is_dir()
                and not self.app_path.is_symlink()
            ):
                self.run_systemctl("start", check=False)
                self.status(
                    state="failed",
                    phase="recovery",
                    error="interrupted before legacy directory migration",
                    current_version=current_version(self.app_path),
                    finished_at=utc_now(),
                )
                return
            self.status(
                state="failed",
                phase="recovery",
                error=f"previous release missing during recovery: {previous_target}",
                finished_at=utc_now(),
            )
            return

        failed_target_raw = status.get("target_dir")
        failed_target = Path(failed_target_raw) if failed_target_raw else None
        self.rollback_to(
            previous_target,
            previous_version,
            failed_target,
            "OTA service restarted during installation",
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--install", action="store_true")
    action.add_argument("--rollback", action="store_true")
    action.add_argument("--recover", action="store_true")
    parser.add_argument("--package", type=Path)
    parser.add_argument("--sha256")
    parser.add_argument("--package-id", default="manual")
    parser.add_argument("--job-id")
    parser.add_argument("--state-dir", type=Path, default=DEFAULT_STATE_DIR)
    parser.add_argument("--xxl-root", type=Path, default=DEFAULT_XXL_ROOT)
    parser.add_argument("--service", default="dvr.service")
    parser.add_argument("--dashboard-service", default="radar-dashboard.service")
    parser.add_argument("--health-timeout", type=int, default=90)
    parser.add_argument("--test-mode", action="store_true")
    args = parser.parse_args()
    if args.job_id is None:
        args.job_id = uuid.uuid4().hex
    if args.install and (args.package is None or args.sha256 is None):
        parser.error("--install requires --package and --sha256")
    return args


def main() -> int:
    args = parse_args()
    installer = Installer(args)
    try:
        if args.install:
            installer.install(
                args.package.resolve(),
                args.sha256,
                args.package_id,
                args.job_id,
            )
        elif args.rollback:
            installer.manual_rollback()
        else:
            installer.recover()
        return 0
    except OTAError as error:
        print(f"helmet OTA: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
