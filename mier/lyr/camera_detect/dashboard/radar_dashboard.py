#!/usr/bin/env python3
"""STM32MP257 radar experiment dashboard.

Only Python's standard library is used so the service can run directly on the
development board. radar_fusion owns radar_data.csv/radar_state.json; this
service serves the UI and appends manual annotations to labels.csv.
"""

from __future__ import annotations

import argparse
import csv
import json
import mimetypes
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


class RadarStore:
    def __init__(self, data_dir: Path) -> None:
        self.data_dir = data_dir
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.state_path = self.data_dir / "radar_state.json"
        self.labels_path = self.data_dir / "labels.csv"
        self.radar_csv_path = self.data_dir / "radar_data.csv"
        self.sensor_events_path = self.data_dir / "sensor_events.csv"
        self.imu_delivery_path = self.data_dir / "imu_delivery.csv"
        self.lock = threading.Lock()
        self.active: dict[str, dict[str, Any]] = {}
        self._restore_active_labels()

    def read_state(self) -> dict[str, Any]:
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
                header = source.readline().decode("utf-8", "replace")
                source.seek(0, 2)
                size = source.tell()
                start = max(len(header.encode("utf-8")), size - 262_144)
                source.seek(start)
                if start > len(header.encode("utf-8")):
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

    def _restore_active_labels(self) -> None:
        if not self.labels_path.exists():
            return
        try:
            with self.labels_path.open("r", encoding="utf-8", newline="") as labels_file:
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


class RadarDashboardServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        address: tuple[str, int],
        handler: type[BaseHTTPRequestHandler],
        store: RadarStore,
        static_dir: Path,
    ) -> None:
        super().__init__(address, handler)
        self.store = store
        self.static_dir = static_dir


class DashboardHandler(BaseHTTPRequestHandler):
    server: RadarDashboardServer

    STATIC_ROUTES = {
        "/": "index.html",
        "/index.html": "index.html",
        "/app.js": "app.js",
        "/styles.css": "styles.css",
    }

    def log_message(self, format_string: str, *args: object) -> None:
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

    def do_POST(self) -> None:  # noqa: N802
        if urlparse(self.path).path != "/api/labels":
            self._error(HTTPStatus.NOT_FOUND, "unknown endpoint")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._error(HTTPStatus.BAD_REQUEST, "invalid content length")
            return
        if length <= 0 or length > 4096:
            self._error(HTTPStatus.BAD_REQUEST, "invalid request body")
            return
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            event_type = str(payload.get("event_type", ""))
            action = str(payload.get("action", ""))
            row = self.server.store.record_label(event_type, action)
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
            self._error(HTTPStatus.BAD_REQUEST, str(exc))
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
        default=Path("/run/media/mmcblk0p1/dvr/radar_experiments"),
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not 1 <= args.port <= 65535:
        raise SystemExit("port must be between 1 and 65535")
    static_dir = Path(__file__).resolve().parent / "static"
    store = RadarStore(args.data_dir.resolve())
    server = RadarDashboardServer(
        (args.host, args.port), DashboardHandler, store, static_dir
    )
    print(
        f"[DASHBOARD] listening on http://{args.host}:{args.port} "
        f"data_dir={store.data_dir}",
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
