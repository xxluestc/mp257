#!/usr/bin/env python3
"""STM32MP257 radar experiment dashboard.

Only Python's standard library is used so the service can run directly on the
development board. radar_fusion owns radar_data.csv/radar_state.json; this
service serves the UI and appends manual annotations to labels.csv.
"""

from __future__ import annotations

import argparse
import codecs
import csv
import json
import mimetypes
import os
import re
import subprocess
import threading
import time
import uuid
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse


EVENT_TYPES = {
    "rear_fast_collision": "正后方快速碰撞",
    "left_rear_fast_collision": "左后方快速碰撞",
    "right_rear_fast_collision": "右后方快速碰撞",
    "approach_no_trigger": "安全接近（预期不告警）",
    "approach_trigger": "危险接近（预期告警）",
}

LABEL_FIELDS = [
    "timestamp",
    "timestamp_ms",
    "event_id",
    "event_type",
    "event_name",
    "action",
    "duration_ms",
    "dangerous_objId",
    "distance_m",
    "velocity_mps",
    "angle_deg",
    "TTC_s",
    "direction",
    "radar_alert",
    "fusion_alert",
]

EMPTY_STATE: dict[str, Any] = {
    "timestamp": None,
    "timestamp_ms": None,
    "has_target": False,
    "obj_count": 0,
    "dangerous_objId": None,
    "distance_m": None,
    "velocity_mps": None,
    "angle_deg": None,
    "filtered_angle_deg": None,
    "TTC_s": None,
    "direction": "UNKNOWN",
    "radar_alert": False,
    "fusion_alert": False,
    "targets": [],
}

SYSTEM_SERVICES = [
    ("radar-dashboard.service", "控制面板", "独立常驻监控与控制"),
    ("dvr-m33.service", "M33 固件", "remoteproc / RPMsg"),
    ("dvr.service", "融合业务", "雷达 · 摄像头 · NPU"),
    ("hostapd.service", "WiFi 热点", "手机接入"),
    ("dnsmasq.service", "DHCP / DNS", "热点地址分配"),
    ("helmet-ota.service", "OTA 服务", "版本与回退"),
]

CRITICAL_PROCESSES = [
    ("radar_fusion", "融合主进程", "radar_fusion"),
    ("hud", "HUD 进程", "hud"),
    ("radar_dashboard.py", "Dashboard", "radar_dashboard.py"),
    ("helmet_ota_server.py", "OTA Server", "helmet_ota_server.py"),
]

CONTROL_TASKS = {
    "dvr": {
        "unit": "dvr.service",
        "name": "融合业务",
        "role": "雷达 · 摄像头 · NPU · HUD",
    },
    "ota": {
        "unit": "helmet-ota.service",
        "name": "OTA 服务",
        "role": "升级包接收与安全回退",
    },
}

MAINTENANCE_ACTIONS = {
    "tf_mount": {
        "name": "挂载 TF 卡",
        "unit": "/dev/mmcblk0p1",
    },
    "tf_eject": {
        "name": "安全弹出 TF 卡",
        "unit": "/dev/mmcblk0p1",
    },
    "project_stop": {
        "name": "安全停止项目",
        "unit": "dvr.service + radar-dashboard.service",
    },
    "system_poweroff": {
        "name": "安全关机",
        "unit": "system",
    },
}

PROTECTED_TASKS = [
    ("dashboard", "radar-dashboard.service", "控制面板", "始终在线，不允许暂停"),
    ("m33", "dvr-m33.service", "M33 核心", "安全告警底座，不允许远程暂停"),
    ("wifi", "hostapd.service", "WiFi 热点", "当前控制链路，不允许远程暂停"),
    ("dhcp", "dnsmasq.service", "DHCP / DNS", "当前控制链路，不允许远程暂停"),
]

BOOT_MILESTONES = [
    ("m33", "M33 running", "[M33_EARLY] M33 running"),
    ("fusion_enter", "融合进程入口", "radar_fusion_main_enter"),
    ("radar_ready", "雷达初始化完成", "radar_initialize_done"),
    ("fusion_ready", "风险核心就绪", "fusion_risk_core_ready"),
    ("audio_ready", "音频输出就绪", "audio_output_ready"),
    ("rpmsg_ready", "RPMsg ready", "rpmsg_ready_sent"),
    ("storage_ready", "板载存储/日志就绪", "business_storage_initialized"),
    ("camera_stream", "摄像头开始采集", "camera_stream_started"),
    ("npu_ready", "NPU 模型就绪", "npu_model_load_done"),
    ("vision_ready", "视觉融合就绪", "fusion_vision_initialized"),
    ("camera_frame", "摄像头首帧", "camera_first_frame"),
]


def read_system_version() -> str:
    """Read the immutable system VERSION from the deployed runtime bundle."""
    version_path = Path(__file__).resolve().parent.parent / "VERSION"
    try:
        version = version_path.read_text(encoding="utf-8").strip()
    except OSError:
        return "source"
    return version[:64] if version else "unknown"


class RadarStore:
    def __init__(self, data_dir: Path) -> None:
        self.data_dir = data_dir
        self.state_path = self.data_dir / "radar_state.json"
        self.labels_path = self.data_dir / "labels.csv"
        self.radar_csv_path = self.data_dir / "radar_data.csv"
        self.sensor_events_path = self.data_dir / "sensor_events.csv"
        self.imu_delivery_path = self.data_dir / "imu_delivery.csv"
        self.control_events_path = self.data_dir / "control_events.csv"
        self.lock = threading.Lock()
        self.storage_lock = threading.Lock()
        self.active: dict[str, dict[str, Any]] = {}
        self._storage_initialized = False
        self._activate_storage()

    def _storage_available(self) -> bool:
        """Keep legacy custom TF paths from writing below an unmounted mountpoint."""
        tf_mount = Path("/run/media/mmcblk0p1")
        try:
            self.data_dir.resolve(strict=False).relative_to(tf_mount)
        except ValueError:
            return True
        return os.path.ismount(tf_mount)

    def _activate_storage(self) -> bool:
        if self._storage_initialized:
            return True
        with self.storage_lock:
            if self._storage_initialized:
                return True
            if not self._storage_available():
                return False
            try:
                self.data_dir.mkdir(parents=True, exist_ok=True)
            except OSError:
                return False
            self._ensure_labels_utf8_bom()
            self._restore_active_labels()
            self._storage_initialized = True
            return True

    def _ensure_labels_utf8_bom(self) -> None:
        """Add a BOM to an existing labels CSV without changing its UTF-8 data."""
        try:
            if not self.labels_path.exists() or self.labels_path.stat().st_size == 0:
                return
            with self.labels_path.open("rb") as labels_file:
                if labels_file.read(len(codecs.BOM_UTF8)) == codecs.BOM_UTF8:
                    return
                labels_file.seek(0)
                content = labels_file.read()
            temporary = self.labels_path.with_name(
                f".{self.labels_path.name}.utf8-bom.tmp"
            )
            with temporary.open("wb") as output:
                output.write(codecs.BOM_UTF8)
                output.write(content)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, self.labels_path)
            print(
                f"[DASHBOARD] added UTF-8 BOM to {self.labels_path}",
                flush=True,
            )
        except OSError as error:
            print(
                f"[DASHBOARD] warning: cannot add UTF-8 BOM to "
                f"{self.labels_path}: {error}",
                flush=True,
            )

    def read_state(self) -> dict[str, Any]:
        self._activate_storage()
        try:
            with self.state_path.open("r", encoding="utf-8") as state_file:
                loaded = json.load(state_file)
            if not isinstance(loaded, dict):
                raise ValueError("state root is not an object")
            state = dict(EMPTY_STATE)
            state.update(loaded)
            return state
        except (FileNotFoundError, OSError, ValueError, json.JSONDecodeError):
            return dict(EMPTY_STATE)

    def active_labels(self) -> dict[str, dict[str, Any]]:
        with self.lock:
            return {key: dict(value) for key, value in self.active.items()}

    @staticmethod
    def _tail_csv(path: Path, max_rows: int = 240) -> list[dict[str, str]]:
        """Read a bounded tail while retaining the CSV header."""
        try:
            with path.open("rb") as source:
                header_bytes = source.readline()
                header = header_bytes.decode("utf-8-sig", "replace")
                source.seek(0, 2)
                size = source.tell()
                header_end = len(header_bytes)
                start = max(header_end, size - 262_144)
                source.seek(start)
                if start > header_end:
                    source.readline()
                tail = source.read().decode("utf-8", "replace")
            lines = tail.splitlines()[-max_rows:]
            if not header.strip() or not lines:
                return []
            return list(csv.DictReader([header.rstrip("\r\n"), *lines]))
        except (FileNotFoundError, OSError, csv.Error):
            return []

    @staticmethod
    def _as_int(value: Any, default: int = 0) -> int:
        try:
            return int(float(value))
        except (TypeError, ValueError):
            return default

    def read_events(self, limit: int = 120) -> dict[str, Any]:
        events: list[dict[str, Any]] = []
        sensor_rows = self._tail_csv(self.sensor_events_path)
        delivery_rows = self._tail_csv(self.imu_delivery_path)
        label_rows = self._tail_csv(self.labels_path, 120)
        radar_rows = self._tail_csv(self.radar_csv_path, 160)

        for row in sensor_rows:
            events.append({
                "timestamp_ms": self._as_int(row.get("timestamp_ms")),
                "source": row.get("source") or "sensor",
                "event_type": row.get("event_type") or "event",
                "status": row.get("status") or "",
                "event_id": row.get("event_id") or "",
                "label": row.get("label") or "",
                "score": row.get("score") or "",
                "count": row.get("count") or "",
                "seq": row.get("seq") or "",
                "reason": row.get("reason") or "",
                "details": row.get("details") or "",
            })

        for row in delivery_rows:
            events.append({
                "timestamp_ms": self._as_int(row.get("timestamp_ms")),
                "source": "hud_delivery",
                "event_type": row.get("app_type") or row.get("m33_type") or "imu",
                "status": row.get("status") or "",
                "event_id": row.get("event_id") or "",
                "label": row.get("stage") or "",
                "score": "",
                "count": "",
                "seq": row.get("seq") or "",
                "reason": row.get("reason") or "",
                "details": f"stage={row.get('stage', '')} bytes={row.get('bytes', '')}",
            })

        for row in label_rows:
            events.append({
                "timestamp_ms": self._as_int(row.get("timestamp_ms")),
                "source": "manual_label",
                "event_type": row.get("event_type") or "label",
                "status": row.get("action") or "",
                "event_id": row.get("event_id") or "",
                "label": row.get("event_name") or "",
                "score": "",
                "count": "",
                "seq": "",
                "reason": "",
                "details": f"direction={row.get('direction', '')} obj={row.get('dangerous_objId', '')}",
            })

        for row in radar_rows:
            if row.get("is_current_dangerous") != "1":
                continue
            events.append({
                "timestamp_ms": self._as_int(row.get("timestamp_ms")),
                "source": "radar",
                "event_type": "dangerous_target",
                "status": "alert" if row.get("radar_alert") == "1" else "tracking",
                "event_id": "",
                "label": f"OBJ {row.get('objId', '')}",
                "score": "",
                "count": "",
                "seq": "",
                "reason": row.get("direction") or "",
                "details": (
                    f"distance={row.get('distance_m', '')}m "
                    f"velocity={row.get('velocity_mps', '')}m/s "
                    f"angle={row.get('angle_deg', '')}deg TTC={row.get('TTC_s', '')}s"
                ),
            })

        events.sort(key=lambda item: item["timestamp_ms"], reverse=True)
        now_ms = int(time.time() * 1000)
        camera = next((e for e in events if e["source"] == "camera_npu"), None)
        imu = next((e for e in events if e["source"] == "imu_m33"), None)
        latest_fall = next(
            (e for e in events
             if e["source"] == "imu_m33" and e["event_type"] == "fall"),
            None,
        )
        fall_chain: list[dict[str, Any]] = []
        if latest_fall is not None:
            event_id = latest_fall["event_id"]
            fall_chain = sorted(
                [e for e in events if event_id and e["event_id"] == event_id],
                key=lambda item: item["timestamp_ms"],
            )

        recent_cutoff = now_ms - 60_000
        road_bumps = sum(
            1 for e in events
            if e["source"] == "imu_m33"
            and e["event_type"] == "road_bump"
            and e["timestamp_ms"] >= recent_cutoff
        )
        return {
            "ok": True,
            "timestamp_ms": now_ms,
            "events": events[:limit],
            "latest_camera": camera,
            "latest_imu": imu,
            "latest_fall": latest_fall,
            "fall_chain": fall_chain,
            "diagnostics": {
                "road_bumps_60s": road_bumps,
                "road_bump_frequent": road_bumps >= 5,
                "sms_ack_supported": False,
                "sms_status": "unconfirmed_no_app_ack",
            },
        }

    def read_key_events(self, limit: int = 12) -> list[dict[str, Any]]:
        """Return de-duplicated critical events from the current boot only."""
        try:
            uptime_s = float(Path("/proc/uptime").read_text(encoding="ascii").split()[0])
        except (OSError, ValueError, IndexError):
            uptime_s = 0.0
        now_ms = int(time.time() * 1000)
        boot_ms = now_ms - int(uptime_s * 1000)
        events = self.read_events(200)["events"]

        for row in self._tail_csv(self.control_events_path, 80):
            events.append({
                "timestamp_ms": self._as_int(row.get("timestamp_ms")),
                "source": "control",
                "event_type": "task_control",
                "status": row.get("result") or "unknown",
                "event_id": "",
                "label": row.get("task_name") or row.get("task") or "控制操作",
                "score": "",
                "count": "",
                "seq": "",
                "reason": row.get("action") or "",
                "details": row.get("details") or "",
            })

        events.sort(key=lambda item: item["timestamp_ms"], reverse=True)
        selected: list[dict[str, Any]] = []
        last_seen: dict[tuple[str, str, str], int] = {}
        for event in events:
            timestamp_ms = self._as_int(event.get("timestamp_ms"))
            if timestamp_ms < boot_ms or timestamp_ms > now_ms + 60_000:
                continue
            source = str(event.get("source") or "")
            status = str(event.get("status") or "")
            if source == "camera_npu" and status != "target":
                continue
            if source == "radar" and status != "alert":
                continue
            if source == "hud_delivery" and status not in {"ok", "sent", "failed"}:
                continue
            if source not in {
                "camera_npu", "radar", "imu_m33", "hud_delivery",
                "manual_label", "a35_dvr", "control",
            }:
                continue

            signature = (
                source,
                str(event.get("event_type") or ""),
                str(event.get("label") or event.get("event_id") or status)
                + (f":{event.get('reason', '')}" if source == "control" else ""),
            )
            previous = last_seen.get(signature)
            cooldown_ms = 30_000 if source == "camera_npu" else 5_000
            if previous is not None and previous - timestamp_ms < cooldown_ms:
                continue
            last_seen[signature] = timestamp_ms
            item = dict(event)
            item["since_boot_s"] = round((timestamp_ms - boot_ms) / 1000, 3)
            selected.append(item)
            if len(selected) >= limit:
                break
        return selected

    def _restore_active_labels(self) -> None:
        if not self.labels_path.exists():
            return
        try:
            with self.labels_path.open(
                "r", encoding="utf-8-sig", newline=""
            ) as labels_file:
                for row in csv.DictReader(labels_file):
                    event_type = row.get("event_type", "")
                    if event_type not in EVENT_TYPES:
                        continue
                    if row.get("action") == "start":
                        self.active[event_type] = {
                            "event_id": row.get("event_id", ""),
                            "timestamp_ms": int(row.get("timestamp_ms", "0") or 0),
                            "event_name": EVENT_TYPES[event_type],
                        }
                    elif row.get("action") == "end":
                        self.active.pop(event_type, None)
        except (OSError, ValueError):
            self.active.clear()

    def record_label(self, event_type: str, action: str) -> dict[str, Any]:
        if not self._activate_storage():
            raise RuntimeError("实验数据存储尚未就绪，请稍后重试")
        if event_type not in EVENT_TYPES:
            raise ValueError("unknown event_type")
        if action not in {"start", "end"}:
            raise ValueError("action must be start or end")

        now_ms = int(time.time() * 1000)
        timestamp = time.strftime(
            "%Y-%m-%dT%H:%M:%S", time.gmtime(now_ms / 1000)
        ) + f".{now_ms % 1000:03d}Z"
        state = self.read_state()

        with self.lock:
            current = self.active.get(event_type)
            if action == "start":
                if current is not None:
                    raise RuntimeError("event already active")
                event_id = uuid.uuid4().hex
                duration_ms: int | str = ""
            else:
                if current is None:
                    raise RuntimeError("event is not active")
                event_id = str(current["event_id"])
                duration_ms = max(0, now_ms - int(current["timestamp_ms"]))

            row = {
                "timestamp": timestamp,
                "timestamp_ms": now_ms,
                "event_id": event_id,
                "event_type": event_type,
                "event_name": EVENT_TYPES[event_type],
                "action": action,
                "duration_ms": duration_ms,
                "dangerous_objId": state.get("dangerous_objId"),
                "distance_m": state.get("distance_m"),
                "velocity_mps": state.get("velocity_mps"),
                "angle_deg": state.get("angle_deg"),
                "TTC_s": state.get("TTC_s"),
                "direction": state.get("direction", "UNKNOWN"),
                "radar_alert": int(bool(state.get("radar_alert"))),
                "fusion_alert": int(bool(state.get("fusion_alert"))),
            }

            needs_header = (
                not self.labels_path.exists() or self.labels_path.stat().st_size == 0
            )
            with self.labels_path.open(
                "a", encoding="utf-8", newline=""
            ) as labels_file:
                writer = csv.DictWriter(labels_file, fieldnames=LABEL_FIELDS)
                if needs_header:
                    labels_file.write("\ufeff")
                    writer.writeheader()
                writer.writerow(row)
                labels_file.flush()

            if action == "start":
                self.active[event_type] = {
                    "event_id": event_id,
                    "timestamp_ms": now_ms,
                    "event_name": EVENT_TYPES[event_type],
                }
            else:
                self.active.pop(event_type, None)
            return row


class SystemMonitor:
    """Low-frequency, read-only system telemetry with bounded subprocess use."""

    SYSTEM_CACHE_SECONDS = 5.0
    BOOT_CACHE_SECONDS = 60.0

    def __init__(self, data_dir: Path) -> None:
        self.data_dir = data_dir
        self.system_lock = threading.Lock()
        self.boot_lock = threading.Lock()
        self._system_cached: dict[str, Any] = {}
        self._system_cached_at = 0.0
        self._boot_cached: dict[str, Any] = {}
        self._boot_cached_at = 0.0
        self._previous_cpu = self._read_cpu_times()
        self._process_samples: dict[int, tuple[int, float]] = {}
        self._clock_ticks = int(os.sysconf("SC_CLK_TCK"))
        self._page_size = int(os.sysconf("SC_PAGE_SIZE"))
        self._cpu_count = max(1, os.cpu_count() or 1)

    @staticmethod
    def _read_text(path: Path, limit: int = 4096) -> str:
        try:
            return path.read_text(encoding="utf-8", errors="replace")[:limit].strip()
        except OSError:
            return ""

    @staticmethod
    def _run(command: list[str], timeout: float = 2.0) -> str:
        try:
            result = subprocess.run(
                command,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout,
            )
            return result.stdout
        except (OSError, subprocess.SubprocessError):
            return ""

    @staticmethod
    def _read_cpu_times() -> tuple[int, int] | None:
        try:
            fields = Path("/proc/stat").read_text(encoding="ascii").splitlines()[0].split()
            values = [int(value) for value in fields[1:]]
            total = sum(values)
            idle = values[3] + (values[4] if len(values) > 4 else 0)
            return total, idle
        except (OSError, ValueError, IndexError):
            return None

    def _cpu_usage(self) -> float | None:
        current = self._read_cpu_times()
        previous = self._previous_cpu
        self._previous_cpu = current
        if current is None or previous is None:
            return None
        total_delta = current[0] - previous[0]
        idle_delta = current[1] - previous[1]
        if total_delta <= 0:
            return None
        return round(max(0.0, min(100.0, 100.0 * (total_delta - idle_delta) / total_delta)), 1)

    def _service_states(self) -> list[dict[str, Any]]:
        units = [item[0] for item in SYSTEM_SERVICES]
        output = self._run([
            "systemctl", "show", *units,
            "-p", "Id",
            "-p", "ActiveState",
            "-p", "SubState",
            "-p", "ActiveEnterTimestampMonotonic",
            "-p", "NRestarts",
        ])
        parsed: dict[str, dict[str, str]] = {}
        current: dict[str, str] = {}
        for line in [*output.splitlines(), ""]:
            if not line.strip():
                unit_id = current.get("Id")
                if unit_id:
                    parsed[unit_id] = current
                current = {}
                continue
            if "=" in line:
                key, value = line.split("=", 1)
                if key == "Id" and current.get("Id"):
                    parsed[current["Id"]] = current
                    current = {}
                current[key] = value

        services: list[dict[str, Any]] = []
        for unit, name, role in SYSTEM_SERVICES:
            values = parsed.get(unit, {})
            active = values.get("ActiveState", "unknown")
            try:
                started_s = round(
                    int(values.get("ActiveEnterTimestampMonotonic", "0")) / 1_000_000,
                    3,
                )
            except ValueError:
                started_s = 0.0
            services.append({
                "unit": unit,
                "name": name,
                "role": role,
                "active": active,
                "sub": values.get("SubState", "unknown"),
                "started_at_boot_s": started_s or None,
                "restarts": int(values.get("NRestarts", "0") or 0),
            })
        return services

    def _critical_processes(self, now: float) -> list[dict[str, Any]]:
        found: dict[str, dict[str, Any]] = {}
        next_samples: dict[int, tuple[int, float]] = {}
        try:
            proc_entries = list(Path("/proc").iterdir())
        except OSError:
            proc_entries = []

        for entry in proc_entries:
            if not entry.name.isdigit():
                continue
            pid = int(entry.name)
            cmdline = self._read_text(entry / "cmdline").replace("\x00", " ")
            comm = self._read_text(entry / "comm", 256)
            match = next(
                (item for item in CRITICAL_PROCESSES
                 if item[0] == comm or item[2] in cmdline),
                None,
            )
            if match is None or match[0] in found:
                continue
            try:
                stat = self._read_text(entry / "stat", 8192)
                tail = stat[stat.rfind(")") + 2:].split()
                state = tail[0]
                ticks = int(tail[11]) + int(tail[12])
                rss_bytes = int(tail[21]) * self._page_size
            except (ValueError, IndexError):
                continue
            previous = self._process_samples.get(pid)
            cpu_percent = 0.0
            if previous is not None and now > previous[1]:
                cpu_percent = (
                    (ticks - previous[0]) / self._clock_ticks /
                    (now - previous[1]) * 100.0
                )
            next_samples[pid] = (ticks, now)
            found[match[0]] = {
                "key": match[0],
                "name": match[1],
                "pid": pid,
                "state": state,
                "cpu_percent": round(max(0.0, cpu_percent), 1),
                "rss_mib": round(rss_bytes / 1024 / 1024, 1),
            }

        self._process_samples = next_samples
        return [
            found.get(key, {
                "key": key,
                "name": name,
                "pid": None,
                "state": "missing",
                "cpu_percent": 0.0,
                "rss_mib": 0.0,
            })
            for key, name, _pattern in CRITICAL_PROCESSES
        ]

    def _memory(self) -> dict[str, Any]:
        values: dict[str, int] = {}
        try:
            for line in Path("/proc/meminfo").read_text(encoding="ascii").splitlines():
                key, raw = line.split(":", 1)
                values[key] = int(raw.strip().split()[0])
        except (OSError, ValueError, IndexError):
            pass
        total = values.get("MemTotal", 0)
        available = values.get("MemAvailable", 0)
        used = max(0, total - available)
        return {
            "total_mib": round(total / 1024),
            "used_mib": round(used / 1024),
            "used_percent": round(used * 100 / total, 1) if total else None,
        }

    def _cpu_frequency_mhz(self) -> float | None:
        frequencies: list[int] = []
        for path in Path("/sys/devices/system/cpu").glob("cpu[0-9]*/cpufreq/scaling_cur_freq"):
            try:
                frequencies.append(int(path.read_text(encoding="ascii").strip()))
            except (OSError, ValueError):
                continue
        return round(sum(frequencies) / len(frequencies) / 1000) if frequencies else None

    def _temperature(self) -> dict[str, Any]:
        readings: list[tuple[float, str]] = []
        for zone in Path("/sys/class/thermal").glob("thermal_zone*"):
            try:
                temperature = int((zone / "temp").read_text(encoding="ascii").strip()) / 1000
                zone_type = self._read_text(zone / "type", 128) or zone.name
                if -40 <= temperature <= 160:
                    readings.append((temperature, zone_type))
            except (OSError, ValueError):
                continue
        if not readings:
            return {"celsius": None, "source": "unavailable"}
        temperature, source = max(readings)
        return {"celsius": round(temperature, 1), "source": source}

    def _storage(self) -> dict[str, Any]:
        tf_mount = Path("/run/media/mmcblk0p1")
        try:
            self.data_dir.resolve(strict=False).relative_to(tf_mount)
            if not os.path.ismount(tf_mount):
                return {"mounted": False, "total_gib": None,
                        "used_gib": None, "used_percent": None}
        except ValueError:
            pass
        try:
            stat = os.statvfs(self.data_dir)
            total = stat.f_blocks * stat.f_frsize
            available = stat.f_bavail * stat.f_frsize
            used = max(0, total - available)
            return {
                "mounted": True,
                "total_gib": round(total / 1024 ** 3, 1),
                "used_gib": round(used / 1024 ** 3, 1),
                "used_percent": round(used * 100 / total, 1) if total else None,
            }
        except OSError:
            return {"mounted": False, "total_gib": None, "used_gib": None, "used_percent": None}

    def _collect_system(self) -> dict[str, Any]:
        now = time.monotonic()
        uptime_text = self._read_text(Path("/proc/uptime"))
        try:
            uptime_s = round(float(uptime_text.split()[0]), 1)
        except (ValueError, IndexError):
            uptime_s = 0.0
        try:
            load_1m, load_5m, load_15m = os.getloadavg()
        except OSError:
            load_1m = load_5m = load_15m = 0.0
        services = self._service_states()
        processes = self._critical_processes(now)
        return {
            "ok": True,
            "timestamp_ms": int(time.time() * 1000),
            "refresh_interval_s": self.SYSTEM_CACHE_SECONDS,
            "uptime_s": uptime_s,
            "cpu": {
                "usage_percent": self._cpu_usage(),
                "count": self._cpu_count,
                "frequency_mhz": self._cpu_frequency_mhz(),
                "load_1m": round(load_1m, 2),
                "load_5m": round(load_5m, 2),
                "load_15m": round(load_15m, 2),
            },
            "memory": self._memory(),
            "temperature": self._temperature(),
            "storage": self._storage(),
            "services": services,
            "processes": processes,
            "devices": {
                "rpmsg": Path("/dev/ttyRPMSG0").exists(),
                "radar_uart": Path("/dev/ttySTM1").exists(),
                "ble_uart": Path("/dev/ttySTM0").exists(),
                "camera": Path("/dev/video7").exists(),
            },
            "remoteproc": self._read_text(Path("/sys/class/remoteproc/remoteproc0/state"), 64) or "unknown",
        }

    def system(self) -> dict[str, Any]:
        with self.system_lock:
            now = time.monotonic()
            if self._system_cached and now - self._system_cached_at < self.SYSTEM_CACHE_SECONDS:
                return dict(self._system_cached)
            self._system_cached = self._collect_system()
            self._system_cached_at = now
            return dict(self._system_cached)

    def _collect_boot(self) -> dict[str, Any]:
        systemd_time = self._run(["systemd-analyze", "time"]).strip()
        journal = self._run([
            "journalctl", "-b", "-u", "dvr-m33.service", "-u", "dvr.service",
            "-o", "short-monotonic", "--no-pager",
        ], timeout=3.0)
        ansi = re.compile(r"\x1b\[[0-9;]*m")
        timestamp_re = re.compile(r"^\[\s*([0-9]+(?:\.[0-9]+)?)\].*?:\s(.*)$")
        milestone_times: dict[str, float] = {}
        key_logs: list[dict[str, Any]] = []
        for raw_line in journal.splitlines():
            line = ansi.sub("", raw_line)
            match = timestamp_re.match(line)
            if match is None:
                continue
            seconds = float(match.group(1))
            message = match.group(2).strip()
            for key, _label, marker in BOOT_MILESTONES:
                if key not in milestone_times and marker in message:
                    milestone_times[key] = seconds
            if (
                "[启动]" in message or "[告警]" in message or
                "Initialization complete" in message or
                "RPMsg ready" in message or "OLED 初始化成功" in message or
                re.search(r"error|failed|warning|错误|失败|警告", message, re.IGNORECASE)
            ):
                key_logs.append({"time_s": round(seconds, 3), "message": message[:240]})

        milestones = [
            {
                "key": key,
                "label": label,
                "time_s": round(milestone_times[key], 3) if key in milestone_times else None,
                "ready": key in milestone_times,
            }
            for key, label, _marker in BOOT_MILESTONES
        ]
        return {
            "ok": True,
            "timestamp_ms": int(time.time() * 1000),
            "refresh_interval_s": self.BOOT_CACHE_SECONDS,
            "boot_id": self._read_text(Path("/proc/sys/kernel/random/boot_id"), 128),
            "systemd_time": systemd_time,
            "milestones": milestones,
            "logs": list(reversed(key_logs[-18:])),
        }

    def boot(self) -> dict[str, Any]:
        with self.boot_lock:
            now = time.monotonic()
            if self._boot_cached and now - self._boot_cached_at < self.BOOT_CACHE_SECONDS:
                return dict(self._boot_cached)
            self._boot_cached = self._collect_boot()
            self._boot_cached_at = now
            return dict(self._boot_cached)


class TaskController:
    """Whitelist-only systemd control plane with persistent operation audit."""

    AUDIT_FIELDS = [
        "timestamp", "timestamp_ms", "task", "task_name", "unit",
        "action", "result", "details",
    ]

    def __init__(self, data_dir: Path) -> None:
        self.audit_path = data_dir / "control_events.csv"
        self.lock = threading.Lock()
        self.control_token = uuid.uuid4().hex
        self.project_root = Path(__file__).resolve().parent.parent

    @staticmethod
    def _unit_state(unit: str) -> dict[str, Any]:
        output = SystemMonitor._run([
            "systemctl", "show", unit,
            "-p", "ActiveState", "-p", "SubState", "-p", "MainPID",
        ])
        values: dict[str, str] = {}
        for line in output.splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                values[key] = value
        try:
            main_pid = int(values.get("MainPID", "0") or 0)
        except ValueError:
            main_pid = 0
        return {
            "active": values.get("ActiveState", "unknown"),
            "sub": values.get("SubState", "unknown"),
            "pid": main_pid or None,
        }

    def _recent_audit(self, limit: int = 8) -> list[dict[str, Any]]:
        rows = RadarStore._tail_csv(self.audit_path, max_rows=40)
        return list(reversed(rows[-limit:]))

    def status(self) -> dict[str, Any]:
        dashboard_state = self._unit_state("radar-dashboard.service")
        protected = []
        for key, unit, name, role in PROTECTED_TASKS:
            protected.append({
                "key": key,
                "unit": unit,
                "name": name,
                "role": role,
                "controllable": False,
                **self._unit_state(unit),
            })
        tasks = []
        for key, item in CONTROL_TASKS.items():
            tasks.append({
                "key": key,
                "unit": item["unit"],
                "name": item["name"],
                "role": item["role"],
                "controllable": True,
                **self._unit_state(item["unit"]),
            })
        tf_device = Path("/dev/mmcblk0p1")
        tf_mount = Path("/run/media/mmcblk0p1")
        tf_mounted = os.path.ismount(tf_mount)
        return {
            "ok": True,
            "timestamp_ms": int(time.time() * 1000),
            "refresh_interval_s": 5,
            "controls_enabled": dashboard_state["active"] == "active",
            "control_token": self.control_token,
            "protected": protected,
            "tasks": tasks,
            "maintenance": {
                "tf_inserted": tf_device.exists(),
                "tf_mounted": tf_mounted,
                "tf_device": str(tf_device),
                "tf_mount": str(tf_mount),
                "recording_storage": "/usr/local/helmet/dvr",
            },
            "audit": self._recent_audit(),
        }

    def _record(
        self, task: str, action: str, result: str, details: str
    ) -> None:
        item = CONTROL_TASKS.get(task) or MAINTENANCE_ACTIONS.get(task)
        if item is None:
            raise ValueError(f"unknown control audit task: {task}")
        now_ms = int(time.time() * 1000)
        row = {
            "timestamp": time.strftime(
                "%Y-%m-%dT%H:%M:%S", time.gmtime(now_ms / 1000)
            ) + f".{now_ms % 1000:03d}Z",
            "timestamp_ms": now_ms,
            "task": task,
            "task_name": item["name"],
            "unit": item["unit"],
            "action": action,
            "result": result,
            "details": details[:240],
        }
        try:
            tf_mount = Path("/run/media/mmcblk0p1")
            try:
                self.audit_path.resolve(strict=False).relative_to(tf_mount)
                if not os.path.ismount(tf_mount):
                    raise OSError("TF storage is not mounted")
            except ValueError:
                pass
            self.audit_path.parent.mkdir(parents=True, exist_ok=True)
            needs_header = not self.audit_path.exists() or self.audit_path.stat().st_size == 0
            with self.audit_path.open("a", encoding="utf-8", newline="") as audit_file:
                writer = csv.DictWriter(audit_file, fieldnames=self.AUDIT_FIELDS)
                if needs_header:
                    writer.writeheader()
                writer.writerow(row)
                audit_file.flush()
                os.fsync(audit_file.fileno())
        except OSError as exc:
            print(f"[CONTROL] audit write failed: {exc}", flush=True)
        print(
            f"[CONTROL] task={task} action={action} result={result} {details}",
            flush=True,
        )

    def perform(
        self, task: str, action: str, confirmation: str, token: str
    ) -> dict[str, Any]:
        if token != self.control_token:
            raise PermissionError("控制令牌无效，请刷新页面后重试")
        if task not in CONTROL_TASKS:
            raise ValueError("该任务不在远程控制白名单中")
        if action not in {"run", "pause"}:
            raise ValueError("action 必须为 run 或 pause")
        if confirmation != f"{task}:{action}":
            raise PermissionError("操作确认不匹配")

        with self.lock:
            dashboard = self._unit_state("radar-dashboard.service")
            if dashboard["active"] != "active":
                raise RuntimeError("独立 Dashboard 服务未就绪，拒绝执行控制操作")
            item = CONTROL_TASKS[task]
            verb = "start" if action == "run" else "stop"
            try:
                result = subprocess.run(
                    ["systemctl", verb, item["unit"]],
                    check=False,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=30,
                )
            except (OSError, subprocess.SubprocessError) as exc:
                details = str(exc)
                self._record(task, action, "failed", details)
                raise RuntimeError(f"systemd 操作失败：{details}") from exc
            details = result.stdout.strip() or f"systemctl {verb} completed"
            if result.returncode != 0:
                self._record(task, action, "failed", details)
                raise RuntimeError(details)
            state = self._unit_state(item["unit"])
            self._record(task, action, "ok", details)
            return {
                "ok": True,
                "task": task,
                "action": action,
                "state": state,
            }

    def perform_maintenance(
        self, action: str, confirmation: str, token: str
    ) -> dict[str, Any]:
        if token != self.control_token:
            raise PermissionError("控制令牌无效，请刷新页面后重试")
        if action not in MAINTENANCE_ACTIONS:
            raise ValueError("该维护动作不在远程控制白名单中")
        if confirmation != f"maintenance:{action}":
            raise PermissionError("维护操作确认不匹配")

        storage_script = self.project_root / "scripts" / "tf_card_control.sh"
        stop_script = self.project_root / "scripts" / "project_safe_stop.sh"
        with self.lock:
            if action in {"tf_mount", "tf_eject"}:
                verb = "mount" if action == "tf_mount" else "eject"
                try:
                    result = subprocess.run(
                        [str(storage_script), verb],
                        check=False,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT,
                        text=True,
                        encoding="utf-8",
                        errors="replace",
                        timeout=30,
                    )
                except (OSError, subprocess.SubprocessError) as exc:
                    details = str(exc)
                    self._record(action, verb, "failed", details)
                    raise RuntimeError(f"TF 操作失败：{details}") from exc
                details = result.stdout.strip() or f"TF {verb} completed"
                if result.returncode != 0:
                    self._record(action, verb, "failed", details)
                    raise RuntimeError(details)
                self._record(action, verb, "ok", details)
                return {"ok": True, "action": action, "details": details}

            mode = "stop" if action == "project_stop" else "poweroff"
            try:
                environment = dict(os.environ)
                environment["SAFE_STOP_DELAY_SEC"] = "2"
                subprocess.Popen(
                    [str(stop_script), mode],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    close_fds=True,
                    start_new_session=True,
                    env=environment,
                )
            except OSError as exc:
                self._record(action, mode, "failed", str(exc))
                raise RuntimeError(f"安全操作调度失败：{exc}") from exc
            details = "scheduled after 2 seconds; storage sync is mandatory"
            self._record(action, mode, "ok", details)
            return {
                "ok": True,
                "action": action,
                "scheduled": True,
                "delay_seconds": 2,
            }


class RadarDashboardServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        address: tuple[str, int],
        handler: type[BaseHTTPRequestHandler],
        store: RadarStore,
        monitor: SystemMonitor,
        controller: TaskController,
        static_dir: Path,
        system_version: str,
    ) -> None:
        super().__init__(address, handler)
        self.store = store
        self.monitor = monitor
        self.controller = controller
        self.static_dir = static_dir
        self.system_version = system_version
        self._request_log_lock = threading.Lock()
        self._request_log_times: dict[str, float] = {}

    def should_log_request(self, path: str, interval_s: float) -> bool:
        """Rate-limit successful high-frequency polling logs per endpoint."""
        now = time.monotonic()
        with self._request_log_lock:
            previous = self._request_log_times.get(path, 0.0)
            if now - previous < interval_s:
                return False
            self._request_log_times[path] = now
            return True


class DashboardHandler(BaseHTTPRequestHandler):
    server: RadarDashboardServer

    STATIC_ROUTES = {
        "/": "index.html",
        "/index.html": "index.html",
        "/app.js": "app.js",
        "/styles.css": "styles.css",
    }

    QUIET_GET_ROUTES = {
        "/api/state": 60.0,
        "/api/events": 60.0,
        "/api/system": 60.0,
        "/api/boot": 60.0,
        "/api/control": 60.0,
    }

    def log_message(self, format_string: str, *args: object) -> None:
        path = urlparse(self.path).path
        try:
            status = int(str(args[1]))
        except (IndexError, TypeError, ValueError):
            status = 500
        quiet_interval = self.QUIET_GET_ROUTES.get(path)
        if (
            self.command == "GET"
            and status < 400
            and quiet_interval is not None
            and not self.server.should_log_request(path, quiet_interval)
        ):
            return
        print(
            f"[DASHBOARD] {self.address_string()} "
            f"{format_string % args}",
            flush=True,
        )

    def _json_response(
        self, payload: dict[str, Any], status: HTTPStatus = HTTPStatus.OK
    ) -> None:
        body = json.dumps(
            payload, ensure_ascii=False, separators=(",", ":")
        ).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _error(self, status: HTTPStatus, message: str) -> None:
        self._json_response({"ok": False, "error": message}, status)

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path
        if path == "/api/state":
            state = self.server.store.read_state()
            now_ms = int(time.time() * 1000)
            state_ms = state.get("timestamp_ms")
            age_ms = (
                max(0, now_ms - int(state_ms))
                if isinstance(state_ms, (int, float))
                else None
            )
            state["age_ms"] = age_ms
            state["stale"] = age_ms is None or age_ms > 3000
            state["dashboard_time_ms"] = now_ms
            # Keep app_version temporarily for older Dashboard clients.
            state["app_version"] = self.server.system_version
            state["system_version"] = self.server.system_version
            state["active_labels"] = self.server.store.active_labels()
            self._json_response(state)
            return
        if path == "/api/events":
            query = parse_qs(parsed.query)
            try:
                limit = max(20, min(200, int(query.get("limit", ["120"])[0])))
            except ValueError:
                limit = 120
            self._json_response(self.server.store.read_events(limit))
            return
        if path == "/api/system":
            self._json_response(self.server.monitor.system())
            return
        if path == "/api/boot":
            payload = self.server.monitor.boot()
            payload["runtime_events"] = self.server.store.read_key_events(12)
            payload["event_refresh_interval_s"] = 10
            self._json_response(payload)
            return
        if path == "/api/control":
            self._json_response(self.server.controller.status())
            return
        if path == "/api/labels/active":
            self._json_response(
                {"ok": True, "active": self.server.store.active_labels()}
            )
            return

        static_name = self.STATIC_ROUTES.get(path)
        if static_name is None:
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        static_path = self.server.static_dir / static_name
        try:
            body = static_path.read_bytes()
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        content_type = mimetypes.guess_type(static_path.name)[0]
        self.send_response(HTTPStatus.OK)
        self.send_header(
            "Content-Type", f"{content_type or 'application/octet-stream'}; charset=utf-8"
        )
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self) -> dict[str, Any]:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as exc:
            raise ValueError("invalid content length") from exc
        if length <= 0 or length > 4096:
            raise ValueError("invalid request body")
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError("invalid JSON body") from exc
        if not isinstance(payload, dict):
            raise ValueError("JSON body must be an object")
        return payload

    def do_POST(self) -> None:  # noqa: N802
        path = urlparse(self.path).path
        if path not in {"/api/labels", "/api/control", "/api/maintenance"}:
            self._error(HTTPStatus.NOT_FOUND, "unknown endpoint")
            return
        try:
            payload = self._read_json_body()
            if path in {"/api/control", "/api/maintenance"}:
                origin = self.headers.get("Origin", "")
                host = self.headers.get("Host", "")
                if origin and urlparse(origin).netloc != host:
                    raise PermissionError("拒绝跨站控制请求")
            if path == "/api/control":
                result = self.server.controller.perform(
                    str(payload.get("task", "")),
                    str(payload.get("action", "")),
                    str(payload.get("confirmation", "")),
                    str(payload.get("control_token", "")),
                )
                self._json_response(result)
                return
            if path == "/api/maintenance":
                result = self.server.controller.perform_maintenance(
                    str(payload.get("action", "")),
                    str(payload.get("confirmation", "")),
                    str(payload.get("control_token", "")),
                )
                self._json_response(result)
                return
            event_type = str(payload.get("event_type", ""))
            action = str(payload.get("action", ""))
            row = self.server.store.record_label(event_type, action)
        except ValueError as exc:
            self._error(HTTPStatus.BAD_REQUEST, str(exc))
            return
        except PermissionError as exc:
            self._error(HTTPStatus.FORBIDDEN, str(exc))
            return
        except RuntimeError as exc:
            self._error(HTTPStatus.CONFLICT, str(exc))
            return
        except OSError as exc:
            self._error(HTTPStatus.INTERNAL_SERVER_ERROR, str(exc))
            return
        self._json_response(
            {
                "ok": True,
                "label": row,
                "active": self.server.store.active_labels(),
            },
            HTTPStatus.CREATED,
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Radar experiment dashboard")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument(
        "--data-dir",
        type=Path,
        default=Path("/usr/local/helmet/radar_experiments"),
    )
    parser.add_argument("--pid-file", type=Path, default=None)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not 1 <= args.port <= 65535:
        raise SystemExit("port must be between 1 and 65535")
    static_dir = Path(__file__).resolve().parent / "static"
    system_version = read_system_version()
    store = RadarStore(args.data_dir.resolve())
    monitor = SystemMonitor(store.data_dir)
    controller = TaskController(store.data_dir)
    server = RadarDashboardServer(
        (args.host, args.port), DashboardHandler, store, monitor, controller,
        static_dir, system_version
    )
    if args.pid_file is not None:
        try:
            args.pid_file.write_text(f"{os.getpid()}\n", encoding="ascii")
        except OSError as exc:
            print(f"[DASHBOARD] pid file write failed: {exc}", flush=True)
    print(
        f"[DASHBOARD] listening on http://{args.host}:{args.port} "
        f"data_dir={store.data_dir} system_version={system_version}",
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        if args.pid_file is not None:
            try:
                if args.pid_file.read_text(encoding="ascii").strip() == str(os.getpid()):
                    args.pid_file.unlink()
            except OSError:
                pass


if __name__ == "__main__":
    main()
