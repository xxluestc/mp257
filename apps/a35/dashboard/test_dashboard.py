#!/usr/bin/env python3
"""Fast host-side regression checks for Dashboard routes and static structure."""

from __future__ import annotations

import json
import re
import tempfile
import time
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from urllib.request import ProxyHandler, Request, build_opener

from radar_dashboard import (
    DashboardHandler,
    RadarDashboardServer,
    RadarStore,
    TaskController,
)


class FakeMonitor:
    def system(self) -> dict[str, object]:
        return {"ok": True, "services": [], "processes": [], "cpu": {}}

    def boot(self) -> dict[str, object]:
        return {"ok": True, "milestones": [], "logs": []}


class FakeController:
    def status(self) -> dict[str, object]:
        return {
            "ok": True,
            "controls_enabled": True,
            "control_token": "test-token",
            "protected": [],
            "tasks": [],
            "audit": [],
        }

    def perform(
        self, task: str, action: str, confirmation: str, token: str
    ) -> dict[str, object]:
        if token != "test-token" or confirmation != f"{task}:{action}":
            raise PermissionError("invalid test control")
        return {"ok": True, "task": task, "action": action}

    def perform_maintenance(
        self, action: str, confirmation: str, token: str
    ) -> dict[str, object]:
        if token != "test-token" or confirmation != f"maintenance:{action}":
            raise PermissionError("invalid test maintenance control")
        return {"ok": True, "action": action}


class DashboardTest(unittest.TestCase):
    def test_manual_labels_are_utf8_bom_and_close_cleanly(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            store = RadarStore(Path(temporary))
            started = store.record_label("left_rear_fast_collision", "start")
            finished = store.record_label("left_rear_fast_collision", "end")
            content = store.labels_path.read_bytes()
            self.assertTrue(content.startswith(b"\xef\xbb\xbf"))
            text = content.decode("utf-8-sig")
            self.assertIn("左后方快速碰撞", text)
            self.assertEqual(started["event_id"], finished["event_id"])
            self.assertEqual(store.active_labels(), {})

    def test_control_audit_appears_in_current_boot_events(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            store = RadarStore(Path(temporary))
            controller = TaskController(store.data_dir)
            now_s = 1_700_000_000.0
            original_read_text = Path.read_text

            def read_text(path: Path, *args: object, **kwargs: object) -> str:
                if path == Path("/proc/uptime"):
                    return "60.00 0.00\n"
                return original_read_text(path, *args, **kwargs)

            # A fixed boot window makes this test independent of the host OS
            # and checks that events from the previous boot are excluded.
            with patch("radar_dashboard.time.time", return_value=now_s - 120):
                controller._record("dvr", "pause", "ok", "previous boot")
            with patch("radar_dashboard.time.time", return_value=now_s):
                controller._record("ota", "pause", "ok", "test audit")
                controller._record("ota", "run", "ok", "test audit")
                with patch.object(Path, "read_text", autospec=True, side_effect=read_text):
                    events = store.read_key_events(4)
            self.assertEqual(len(events), 2)
            self.assertEqual(events[0]["source"], "control")
            self.assertEqual({event["reason"] for event in events}, {"pause", "run"})

    def test_controller_rejects_non_whitelisted_task(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            controller = TaskController(Path(temporary))
            with self.assertRaises(ValueError):
                controller.perform(
                    "radar-dashboard", "pause", "radar-dashboard:pause",
                    controller.control_token,
                )

    def test_control_status_reports_actual_tf_dvr_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            controller = TaskController(Path(temporary))
            controller._tf_status = lambda: {  # type: ignore[method-assign]
                "tf_inserted": True,
                "tf_mounted": True,
                "tf_status": "mounted",
                "tf_device": "/dev/mmcblk0",
                "tf_mount": "/run/media/mmcblk0",
            }
            controller._unit_state = lambda unit: {  # type: ignore[method-assign]
                "active": "active", "sub": "running", "pid": 123,
            }
            status = controller.status()
            maintenance = status["maintenance"]
            self.assertEqual(
                maintenance["recording_storage"], "/run/media/mmcblk0/dvr"
            )
            self.assertTrue(maintenance["recording_storage_ready"])

    def test_safe_tf_eject_stops_project_before_unmount(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            controller = TaskController(Path(temporary))
            calls: list[list[str]] = []

            def fake_run(command: list[str], **_kwargs: object) -> SimpleNamespace:
                calls.append(command)
                return SimpleNamespace(returncode=0, stdout="ok")

            with patch("radar_dashboard.subprocess.run", side_effect=fake_run):
                result = controller.perform_maintenance(
                    "tf_eject", "maintenance:tf_eject", controller.control_token
                )
            self.assertTrue(result["ok"])
            self.assertEqual(calls[0][-1], "stop")
            self.assertEqual(calls[1][-1], "eject")

    def test_static_ids_are_unique_and_views_exist(self) -> None:
        html = (Path(__file__).parent / "static" / "index.html").read_text(
            encoding="utf-8"
        )
        ids = re.findall(r'\bid="([^"]+)"', html)
        self.assertEqual(len(ids), len(set(ids)))
        for view in (
            "view-radar", "view-sensors", "view-system", "view-boot", "view-control"
        ):
            self.assertIn(view, ids)

    def test_ota_demo_theme_is_strictly_version_gated(self) -> None:
        static_dir = Path(__file__).parent / "static"
        app = (static_dir / "app.js").read_text(encoding="utf-8")
        styles = (static_dir / "styles.css").read_text(encoding="utf-8")
        self.assertIn('systemVersion === "1.0.8"', app)
        self.assertIn('body[data-release="1.0.8"]', styles)
        self.assertIn('content: "OTA DEMO"', styles)

    def test_dashboard_exposes_colored_dvr_lifecycle_events(self) -> None:
        static_dir = Path(__file__).parent / "static"
        app = (static_dir / "app.js").read_text(encoding="utf-8")
        html = (static_dir / "index.html").read_text(encoding="utf-8")
        styles = (static_dir / "styles.css").read_text(encoding="utf-8")
        for label in (
            "事件录像已触发", "录像编码进行中", "视频已校验并保存", "视频保存失败"
        ):
            self.assertIn(label, app)
        self.assertIn("关键事件与录像日志", html)
        for tone in ("tone-failed", "tone-triggered", "tone-processing", "tone-saved"):
            self.assertIn(tone, styles)

    def test_read_only_api_routes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            static_dir = Path(__file__).parent / "static"
            store = RadarStore(root)
            server = RadarDashboardServer(
                ("127.0.0.1", 0),
                DashboardHandler,
                store,
                FakeMonitor(),  # type: ignore[arg-type]
                FakeController(),  # type: ignore[arg-type]
                static_dir,
                "test",
            )
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{server.server_port}"
            opener = build_opener(ProxyHandler({}))
            try:
                for route in (
                    "/api/state", "/api/events", "/api/system", "/api/boot",
                    "/api/control",
                ):
                    with opener.open(base + route, timeout=2) as response:
                        payload = json.load(response)
                    self.assertIsInstance(payload, dict)
                    if route == "/api/state":
                        self.assertEqual(payload.get("system_version"), "test")
                with opener.open(base + "/", timeout=2) as response:
                    html = response.read().decode("utf-8")
                self.assertIn("系统状态", html)
                self.assertIn("关键时间", html)
                self.assertIn("任务控制", html)
                self.assertIn("设备运维", html)
                self.assertIn("安全关机", html)
                self.assertIn("系统版本", html)
                self.assertNotIn(">APP <", html)
                # A live application heartbeat must not disguise a missing radar.
                store.state_path.write_text(json.dumps({
                    "timestamp_ms": int(time.time() * 1000), "radar_available": False,
                }), encoding="utf-8")
                with opener.open(base + "/api/state", timeout=2) as response:
                    self.assertTrue(json.load(response)["stale"])
                control_request = Request(
                    base + "/api/control",
                    data=json.dumps({
                        "task": "dvr",
                        "action": "pause",
                        "confirmation": "dvr:pause",
                        "control_token": "test-token",
                    }).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    method="POST",
                )
                with opener.open(control_request, timeout=2) as response:
                    control_result = json.load(response)
                self.assertTrue(control_result.get("ok"))
                maintenance_request = Request(
                    base + "/api/maintenance",
                    data=json.dumps({
                        "action": "tf_mount",
                        "confirmation": "maintenance:tf_mount",
                        "control_token": "test-token",
                    }).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    method="POST",
                )
                with opener.open(maintenance_request, timeout=2) as response:
                    maintenance_result = json.load(response)
                self.assertTrue(maintenance_result.get("ok"))
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
