"use strict";

const EVENTS = [
  ["rear_fast_collision", "正后方快速碰撞", "REAR · FAST · COLLISION"],
  ["left_rear_fast_collision", "左后方快速碰撞", "LEFT REAR · FAST · COLLISION"],
  ["right_rear_fast_collision", "右后方快速碰撞", "RIGHT REAR · FAST · COLLISION"],
  ["approach_no_trigger", "不触发的接近", "APPROACH · EXPECT NO ALERT"],
  ["approach_trigger", "触发的接近", "APPROACH · EXPECT ALERT"],
];

const $ = (id) => document.getElementById(id);
const historyLimit = 180;
const history = {
  distance: [],
  velocity: [],
  angle: [],
  ttc: [],
};
let activeLabels = {};
let lastSampleTimestamp = null;
let toastTimer = null;

const charts = [
  { key: "distance", canvas: $("chart-distance"), color: "#31c7bc", unit: "m" },
  { key: "velocity", canvas: $("chart-velocity"), color: "#6f97ff", unit: "m/s" },
  { key: "angle", canvas: $("chart-angle"), color: "#ffb84d", unit: "°" },
  { key: "ttc", canvas: $("chart-ttc"), color: "#ff5f58", unit: "s" },
];

function finite(value) {
  return typeof value === "number" && Number.isFinite(value);
}

function format(value, digits = 1) {
  return finite(value) ? value.toFixed(digits) : "—";
}

function showToast(message, isError = false) {
  const toast = $("toast");
  toast.textContent = message;
  toast.classList.toggle("error", isError);
  toast.classList.add("visible");
  window.clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => toast.classList.remove("visible"), 2400);
}

function renderEvents() {
  const list = $("event-list");
  list.innerHTML = "";
  for (const [key, name, code] of EVENTS) {
    const isActive = Boolean(activeLabels[key]);
    const row = document.createElement("div");
    row.className = `event-row${isActive ? " active" : ""}`;
    row.dataset.eventType = key;

    const copy = document.createElement("div");
    copy.className = "event-copy";
    const strong = document.createElement("strong");
    strong.textContent = name;
    const detail = document.createElement("span");
    detail.className = "event-detail";
    if (isActive) {
      const elapsed = Math.max(
        0,
        Date.now() - Number(activeLabels[key].timestamp_ms || Date.now())
      );
      detail.textContent = `RECORDING · ${(elapsed / 1000).toFixed(1)} s`;
    } else {
      detail.textContent = code;
    }
    copy.append(strong, detail);

    const button = document.createElement("button");
    button.type = "button";
    button.className = "event-button";
    button.textContent = isActive ? "结束标记" : "开始标记";
    button.setAttribute("aria-pressed", isActive ? "true" : "false");
    button.addEventListener("click", () =>
      recordEvent(key, isActive ? "end" : "start", button)
    );
    row.append(copy, button);
    list.append(row);
  }
}

function updateEventTimers() {
  for (const row of document.querySelectorAll(".event-row.active")) {
    const active = activeLabels[row.dataset.eventType];
    const detail = row.querySelector(".event-detail");
    if (!active || !detail) continue;
    const elapsed = Math.max(
      0,
      Date.now() - Number(active.timestamp_ms || Date.now())
    );
    detail.textContent = `RECORDING · ${(elapsed / 1000).toFixed(1)} s`;
  }
}

async function recordEvent(eventType, action, button) {
  button.disabled = true;
  try {
    const response = await fetch("/api/labels", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ event_type: eventType, action }),
    });
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || "标记写入失败");
    activeLabels = result.active || {};
    renderEvents();
    showToast(action === "start" ? "事件开始已写入 labels.csv" : "事件结束已写入 labels.csv");
  } catch (error) {
    showToast(error.message || "标记写入失败", true);
    button.disabled = false;
  }
}

function updateRadarMarker(state) {
  const marker = $("target-marker");
  if (!state.has_target || !finite(state.filtered_angle_deg)) {
    marker.classList.remove("visible");
    return;
  }
  const angle = Math.max(-65, Math.min(65, state.filtered_angle_deg));
  const distance = finite(state.distance_m) ? state.distance_m : 10;
  const radius = Math.max(35, Math.min(150, distance * 7));
  const radians = (angle * Math.PI) / 180;
  marker.setAttribute("cx", String(180 + Math.sin(radians) * radius));
  marker.setAttribute("cy", String(190 - Math.cos(radians) * radius));
  marker.classList.add("visible");
}

function renderTargets(targets) {
  const body = $("targets-body");
  body.innerHTML = "";
  if (!Array.isArray(targets) || targets.length === 0) {
    body.innerHTML = '<tr class="empty-row"><td colspan="6">当前无雷达目标</td></tr>';
    $("object-count").textContent = "0 TARGETS";
    return;
  }
  $("object-count").textContent = `${targets.length} TARGET${targets.length > 1 ? "S" : ""}`;
  for (const target of targets) {
    const row = document.createElement("tr");
    if (target.is_dangerous) row.className = "dangerous";
    const values = [
      target.objId,
      `${format(target.distance_m)} m`,
      `${format(target.velocity_mps)} m/s`,
      `${format(target.angle_deg)}°`,
      finite(target.TTC_s) ? `${format(target.TTC_s)} s` : "—",
      target.direction || "UNKNOWN",
    ];
    for (const value of values) {
      const cell = document.createElement("td");
      cell.textContent = value;
      row.append(cell);
    }
    body.append(row);
  }
}

function addSample(state) {
  if (state.timestamp_ms === lastSampleTimestamp || !state.has_target) return;
  lastSampleTimestamp = state.timestamp_ms;
  const values = {
    distance: state.distance_m,
    velocity: state.velocity_mps,
    angle: state.filtered_angle_deg,
    ttc: state.TTC_s,
  };
  for (const [key, value] of Object.entries(values)) {
    history[key].push(finite(value) ? value : null);
    if (history[key].length > historyLimit) history[key].shift();
  }
}

function drawChart(chart) {
  const canvas = chart.canvas;
  const rect = canvas.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) return;
  const ratio = Math.min(window.devicePixelRatio || 1, 2);
  canvas.width = Math.round(rect.width * ratio);
  canvas.height = Math.round(rect.height * ratio);
  const ctx = canvas.getContext("2d");
  ctx.scale(ratio, ratio);
  const width = rect.width;
  const height = rect.height;
  const pad = { left: 4, right: 4, top: 8, bottom: 12 };
  const values = history[chart.key];
  const numeric = values.filter(finite);

  ctx.clearRect(0, 0, width, height);
  ctx.strokeStyle = "rgba(130,147,155,.18)";
  ctx.lineWidth = 1;
  for (let line = 1; line < 4; line += 1) {
    const y = pad.top + ((height - pad.top - pad.bottom) * line) / 4;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(width, y);
    ctx.stroke();
  }
  if (numeric.length < 2) return;

  let min = Math.min(...numeric);
  let max = Math.max(...numeric);
  if (Math.abs(max - min) < 0.001) {
    min -= 1;
    max += 1;
  } else {
    const margin = (max - min) * 0.12;
    min -= margin;
    max += margin;
  }

  ctx.strokeStyle = chart.color;
  ctx.lineWidth = 2;
  ctx.lineJoin = "round";
  ctx.beginPath();
  let drawing = false;
  values.forEach((value, index) => {
    if (!finite(value)) {
      drawing = false;
      return;
    }
    const x = pad.left + (index / Math.max(historyLimit - 1, 1)) *
      (width - pad.left - pad.right);
    const y = pad.top + (1 - (value - min) / (max - min)) *
      (height - pad.top - pad.bottom);
    if (!drawing) {
      ctx.moveTo(x, y);
      drawing = true;
    } else {
      ctx.lineTo(x, y);
    }
  });
  ctx.stroke();
}

function renderState(state) {
  const online = !state.stale;
  const chip = $("connection-chip");
  chip.className = `status-chip ${online ? "status-online" : "status-offline"}`;
  chip.querySelector("span").textContent = online ? "雷达数据在线" : "雷达数据超时";
  $("last-update").textContent = state.timestamp
    ? new Date(state.timestamp).toLocaleTimeString("zh-CN", { hour12: false })
    : "--:--:--";

  $("obj-id").textContent = state.dangerous_objId ?? "—";
  $("distance").textContent = format(state.distance_m);
  $("velocity").textContent = format(state.velocity_mps);
  $("angle").textContent = format(state.angle_deg);
  $("ttc").textContent = format(state.TTC_s);
  $("direction").textContent = state.has_target ? (state.direction || "UNKNOWN") : "—";
  $("fusion-alert").textContent = state.fusion_alert ? "已触发" : "未触发";

  const alarm = $("alarm-state");
  alarm.className = `alarm-state ${state.radar_alert ? "alarm-danger" : "alarm-clear"}`;
  alarm.querySelector("strong").textContent = state.radar_alert ? "雷达危险" : "无告警";

  $("chart-distance-value").textContent = `${format(state.distance_m)} m`;
  $("chart-velocity-value").textContent = `${format(state.velocity_mps)} m/s`;
  $("chart-angle-value").textContent = `${format(state.filtered_angle_deg)} °`;
  $("chart-ttc-value").textContent = `${format(state.TTC_s)} s`;

  updateRadarMarker(state);
  renderTargets(state.targets);
  addSample(state);
  charts.forEach(drawChart);

  const nextActiveLabels = state.active_labels || {};
  const previousSignature = JSON.stringify(activeLabels);
  const nextSignature = JSON.stringify(nextActiveLabels);
  activeLabels = nextActiveLabels;
  if (previousSignature !== nextSignature) renderEvents();
}

async function pollState() {
  try {
    const response = await fetch("/api/state", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderState(await response.json());
  } catch (_) {
    const chip = $("connection-chip");
    chip.className = "status-chip status-offline";
    chip.querySelector("span").textContent = "Dashboard 连接中断";
  }
}

renderEvents();
pollState();
window.setInterval(pollState, 500);
window.setInterval(updateEventTimers, 250);
window.addEventListener("resize", () => charts.forEach(drawChart));
