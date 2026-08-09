#!/usr/bin/env python3
"""Fast host-side regression checks for Dashboard routes and static structure."""

from __future__ import annotations

import json
import re
import tempfile
import threading
import unittest
from pathlib import Path
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


class DashboardTest(unittest.TestCase):
    def test_control_audit_appears_in_current_boot_events(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            store = RadarStore(Path(temporary))
            controller = TaskController(store.data_dir)
            controller._record("ota", "pause", "ok", "test audit")
            controller._record("ota", "run", "ok", "test audit")
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
                self.assertIn("系统版本", html)
                self.assertNotIn(">APP <", html)
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
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)


if __name__ == "__main__":
    unittest.main()
