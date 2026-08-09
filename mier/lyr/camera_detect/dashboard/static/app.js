"use strict";

const EVENTS = [
  ["rear_fast_collision", "正后方快速碰撞", "REAR · FAST · COLLISION"],
  ["left_rear_fast_collision", "左后方快速碰撞", "LEFT REAR · FAST · COLLISION"],
  ["right_rear_fast_collision", "右后方快速碰撞", "RIGHT REAR · FAST · COLLISION"],
  ["approach_no_trigger", "安全接近（预期不告警）", "SAFE APPROACH · EXPECT NO ALERT"],
  ["approach_trigger", "危险接近（预期告警）", "DANGEROUS APPROACH · EXPECT ALERT"],
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
let latestEventPayload = null;
let activeView = "radar";
let statePolling = false;
let eventsPolling = false;
let systemPolling = false;
let bootPolling = false;
let controlPolling = false;
let controlToken = "";
let pendingControl = null;

const charts = [
  { key: "distance", canvas: $("chart-distance"), color: "#087f73", unit: "m" },
  { key: "velocity", canvas: $("chart-velocity"), color: "#315fc0", unit: "m/s" },
  { key: "angle", canvas: $("chart-angle"), color: "#9a5600", unit: "°" },
  { key: "ttc", canvas: $("chart-ttc"), color: "#c63a34", unit: "s" },
];

function activateView(name, moveFocus = false) {
  const nextView = document.querySelector(`#view-${name}`);
  const nextTab = document.querySelector(`[data-view="${name}"]`);
  if (!nextView || !nextTab) return;
  activeView = name;
  for (const tab of document.querySelectorAll(".mode-tab")) {
    const selected = tab === nextTab;
    tab.classList.toggle("active", selected);
    tab.setAttribute("aria-selected", selected ? "true" : "false");
    tab.tabIndex = selected ? 0 : -1;
  }
  for (const view of document.querySelectorAll(".view")) {
    const selected = view === nextView;
    view.hidden = !selected;
    view.classList.toggle("active", selected);
  }
  if (moveFocus) nextTab.focus();
  if (name === "radar") window.requestAnimationFrame(() => charts.forEach(drawChart));
  if (name === "sensors") pollEvents();
  if (name === "system") pollSystem();
  if (name === "boot") pollBoot();
  if (name === "control") pollControl();
  window.history.replaceState(null, "", `#${name}`);
}

function bindTabs() {
  const tabs = [...document.querySelectorAll(".mode-tab")];
  tabs.forEach((tab, index) => {
    tab.addEventListener("click", () => activateView(tab.dataset.view));
    tab.addEventListener("keydown", (event) => {
      if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) return;
      event.preventDefault();
      let target = index;
      if (event.key === "ArrowLeft") target = (index - 1 + tabs.length) % tabs.length;
      if (event.key === "ArrowRight") target = (index + 1) % tabs.length;
      if (event.key === "Home") target = 0;
      if (event.key === "End") target = tabs.length - 1;
      activateView(tabs[target].dataset.view, true);
    });
  });
  const requested = window.location.hash.slice(1);
  activateView(["radar", "sensors", "system", "boot", "control"].includes(requested) ? requested : "radar");
}

function finite(value) {
  return typeof value === "number" && Number.isFinite(value);
}

function format(value, digits = 1) {
  return finite(value) ? value.toFixed(digits) : "—";
}

function angleSign(state) {
  const configured = state && state.thresholds
    ? Number(state.thresholds.angle_sign)
    : -1;
  return configured === 1 ? 1 : -1;
}

function riderAngle(state, sensorAngle) {
  return finite(sensorAngle) ? sensorAngle * angleSign(state) : null;
}

function scopePoint(angle, radius = 174) {
  const clamped = Math.max(-60, Math.min(60, angle));
  const radians = (clamped * Math.PI) / 180;
  return {
    x: 180 + Math.sin(radians) * radius,
    y: 190 - Math.cos(radians) * radius,
  };
}

function sectorPath(startAngle, endAngle) {
  const start = scopePoint(startAngle);
  const end = scopePoint(endAngle);
  return `M180 190 L${start.x.toFixed(1)} ${start.y.toFixed(1)} ` +
    `A174 174 0 0 1 ${end.x.toFixed(1)} ${end.y.toFixed(1)} Z`;
}

function updateRadarGeometry(state) {
  const thresholds = state.thresholds || {};
  let left = finite(Number(thresholds.left_angle_deg))
    ? Number(thresholds.left_angle_deg)
    : -10;
  let right = finite(Number(thresholds.right_angle_deg))
    ? Number(thresholds.right_angle_deg)
    : 10;
  left = Math.max(-58, Math.min(56, left));
  right = Math.max(-56, Math.min(58, right));
  if (left >= right) {
    left = -10;
    right = 10;
  }

  $("scope-sector-left").setAttribute("d", sectorPath(-60, left));
  $("scope-sector-center").setAttribute("d", sectorPath(left, right));
  $("scope-sector-right").setAttribute("d", sectorPath(right, 60));

  const leftPoint = scopePoint(left);
  const rightPoint = scopePoint(right);
  $("scope-left-boundary").setAttribute("x2", leftPoint.x.toFixed(1));
  $("scope-left-boundary").setAttribute("y2", leftPoint.y.toFixed(1));
  $("scope-right-boundary").setAttribute("x2", rightPoint.x.toFixed(1));
  $("scope-right-boundary").setAttribute("y2", rightPoint.y.toFixed(1));
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
  const contact = $("target-contact");
  const marker = $("target-marker");
  if (!state.has_target || !finite(state.filtered_angle_deg)) {
    contact.classList.remove("visible");
    return;
  }
  const angle = riderAngle(state, state.filtered_angle_deg);
  const distance = finite(state.distance_m) ? state.distance_m : 12;
  // 近距离目标仍离开扇形原点足够远，避免被底部方向框遮挡。
  const radius = 70 + (Math.max(0, Math.min(12, distance)) / 12) * 92;
  const point = scopePoint(angle, radius);
  const halo = $("target-halo");
  const caption = $("target-caption");
  marker.setAttribute("cx", point.x.toFixed(1));
  marker.setAttribute("cy", point.y.toFixed(1));
  halo.setAttribute("cx", point.x.toFixed(1));
  halo.setAttribute("cy", point.y.toFixed(1));
  caption.setAttribute("x", (point.x + (point.x > 250 ? -14 : 14)).toFixed(1));
  caption.setAttribute("y", Math.max(17, point.y - 13).toFixed(1));
  caption.setAttribute("text-anchor", point.x > 250 ? "end" : "start");
  caption.textContent =
    `OBJ ${state.dangerous_objId ?? "—"} · ${format(state.distance_m)}m`;
  contact.classList.add("visible");
}

function renderTargets(targets, state) {
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
      `${format(riderAngle(state, target.filtered_angle_deg ?? target.angle_deg))}°`,
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
    angle: riderAngle(state, state.filtered_angle_deg),
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
  ctx.strokeStyle = "rgba(61,88,101,.16)";
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
  $("system-version").textContent = state.system_version || state.app_version || "unknown";
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
  $("chart-angle-value").textContent =
    `${format(riderAngle(state, state.filtered_angle_deg))} °`;
  $("chart-ttc-value").textContent = `${format(state.TTC_s)} s`;

  updateRadarGeometry(state);
  updateRadarMarker(state);
  renderTargets(state.targets, state);
  addSample(state);
  charts.forEach(drawChart);

  const nextActiveLabels = state.active_labels || {};
  const previousSignature = JSON.stringify(activeLabels);
  const nextSignature = JSON.stringify(nextActiveLabels);
  activeLabels = nextActiveLabels;
  if (previousSignature !== nextSignature) renderEvents();
}

function ageText(timestampMs) {
  const age = Math.max(0, Date.now() - Number(timestampMs || 0));
  if (!timestampMs) return "—";
  if (age < 1000) return "刚刚";
  if (age < 60000) return `${Math.floor(age / 1000)} 秒前`;
  return `${Math.floor(age / 60000)} 分钟前`;
}

function setDeliveryStage(name, state, note) {
  const stage = document.querySelector(`[data-stage="${name}"]`);
  if (!stage) return;
  stage.classList.remove("ok", "failed", "unknown");
  stage.classList.add(state);
  stage.querySelector("small").textContent = note;
}

function sourceName(source) {
  return ({
    radar: "RADAR",
    camera_npu: "CAM/NPU",
    a35_dvr: "A35/DVR",
    imu_m33: "M33/IMU",
    a35_hud: "A35→HUD",
    hud_delivery: "HUD→APP",
    manual_label: "LABEL",
    control: "CONTROL",
  })[source] || String(source || "EVENT").toUpperCase();
}

function eventSummary(event) {
  if (event.source === "camera_npu") {
    return event.status === "target"
      ? `检测到 ${event.label || "道路用户"}${event.score ? ` · ${Number(event.score).toFixed(2)}` : ""}`
      : "未检测到道路用户";
  }
  if (event.source === "a35_dvr") {
    return `${event.event_type} · ${event.status}${event.reason ? ` · ${event.reason}` : ""}`;
  }
  if (event.source === "radar") return `${event.label} · ${event.reason || "UNKNOWN"}`;
  if (event.source === "manual_label") return `${event.label} · ${event.status}`;
  if (event.source === "hud_delivery") return `${event.label} · ${event.status}`;
  if (event.source === "control") {
    const action = event.reason === "pause" ? "暂停" : "运行";
    return `${event.label} · ${action} · ${event.status}`;
  }
  return `${event.event_type}${event.reason ? ` · ${event.reason}` : ""}`;
}

function renderTimeline(events) {
  const list = $("timeline-list");
  list.innerHTML = "";
  const sourceCounts = {};
  const selected = [];
  for (const event of events || []) {
    const cap = event.source === "camera_npu" ? 12 : event.source === "radar" ? 10 : 30;
    sourceCounts[event.source] = (sourceCounts[event.source] || 0) + 1;
    if (sourceCounts[event.source] <= cap) selected.push(event);
    if (selected.length >= 6) break;
  }
  if (!selected.length) {
    list.innerHTML = '<p class="timeline-empty">暂无同步事件</p>';
    return;
  }
  for (const event of selected) {
    const row = document.createElement("div");
    row.className = `timeline-row source-${event.source}`;
    const time = document.createElement("time");
    time.textContent = new Date(event.timestamp_ms).toLocaleTimeString("zh-CN", {
      hour12: false, hour: "2-digit", minute: "2-digit", second: "2-digit",
      fractionalSecondDigits: 3,
    });
    const dot = document.createElement("i");
    const content = document.createElement("div");
    const head = document.createElement("div");
    const source = document.createElement("span");
    source.textContent = sourceName(event.source);
    const summary = document.createElement("strong");
    summary.textContent = eventSummary(event);
    head.append(source, summary);
    const detail = document.createElement("small");
    detail.textContent = event.details || `event=${event.event_type}`;
    content.append(head, detail);
    row.append(time, dot, content);
    list.append(row);
  }
}

function renderSensorEvents(payload) {
  latestEventPayload = payload;
  const camera = payload.latest_camera;
  $("camera-state").textContent = camera
    ? (camera.status === "target" ? "检测到道路用户" : "画面无目标")
    : "等待推理";
  $("camera-detail").textContent = camera
    ? (camera.status === "target"
      ? `${camera.label || "目标"} · 置信度 ${camera.score || "—"} · 数量 ${camera.count || 0}`
      : camera.details || "NPU 推理正常")
    : "尚无摄像头检测日志";
  $("camera-age").textContent = ageText(camera?.timestamp_ms);

  const imu = payload.latest_imu;
  $("imu-state").textContent = imu ? imu.event_type : "等待事件";
  $("imu-detail").textContent = imu
    ? `SEQ ${imu.seq || "—"} · ${imu.reason || "无原因字段"}`
    : "尚无 M 核异常上报";
  $("imu-age").textContent = ageText(imu?.timestamp_ms);

  const fall = payload.latest_fall;
  $("fall-event-id").textContent = fall?.event_id || "NO FALL EVENT";
  $("fall-age").textContent = ageText(fall?.timestamp_ms);
  const chain = payload.fall_chain || [];
  const a35 = chain.find((event) => event.source === "a35_hud");
  const hud = chain.find((event) => event.source === "hud_delivery" && event.label === "hud_received");
  const udp = chain.find((event) => event.source === "hud_delivery" && event.label === "app_broadcast");
  setDeliveryStage("m33", fall ? "ok" : "unknown", fall ? "已判断" : "等待");
  setDeliveryStage("a35", a35?.status === "sent" ? "ok" : a35 ? "failed" : "unknown", a35?.status || "等待");
  setDeliveryStage("hud", hud?.status === "ok" ? "ok" : hud ? "failed" : "unknown", hud?.status || "等待");
  setDeliveryStage("udp", udp?.status === "sent" ? "ok" : udp ? "failed" : "unknown", udp?.status || "等待");
  setDeliveryStage("sms", "unknown", "协议无回执");
  $("sms-state").textContent = udp?.status === "sent" ? "已广播，SMS 未确认" : "手机 / SMS 未确认";
  $("sms-detail").textContent = udp
    ? `App UDP：${udp.status}；短信结果无法由开发板确认`
    : "当前协议没有 App 或短信回执";

  const diagnostics = payload.diagnostics || {};
  const banner = $("imu-diagnostic");
  banner.hidden = !diagnostics.road_bump_frequent;
  if (!banner.hidden) {
    banner.textContent = `IMU 标定提示：最近 60 秒出现 ${diagnostics.road_bumps_60s} 次 road_bump，当前静态环境仍可能误报，建议室外采样后再调整 M33 阈值。`;
  }
  renderTimeline(payload.events);
}

function percentText(value) {
  return finite(value) ? `${value.toFixed(1)}%` : "采样中";
}

function uptimeText(seconds) {
  const value = Math.max(0, Number(seconds || 0));
  const days = Math.floor(value / 86400);
  const hours = Math.floor((value % 86400) / 3600);
  const minutes = Math.floor((value % 3600) / 60);
  if (days) return `${days}d ${hours}h`;
  if (hours) return `${hours}h ${minutes}m`;
  return `${minutes}m`;
}

function setHealthValue(id, ok, onlineText = "READY", offlineText = "MISSING") {
  const element = $(id);
  element.textContent = ok ? onlineText : offlineText;
  element.classList.toggle("bad", !ok);
}

function renderSystem(payload) {
  const cpu = payload.cpu || {};
  const memory = payload.memory || {};
  const temperature = payload.temperature || {};
  const storage = payload.storage || {};
  $("cpu-usage").textContent = percentText(cpu.usage_percent);
  $("cpu-frequency").textContent = finite(cpu.frequency_mhz) ? Math.round(cpu.frequency_mhz) : "—";
  $("cpu-load").textContent = finite(cpu.load_1m) ? cpu.load_1m.toFixed(2) : "—";
  $("cpu-count").textContent = `${cpu.count || "—"} cores · 5m ${finite(cpu.load_5m) ? cpu.load_5m.toFixed(2) : "—"}`;
  $("memory-usage").textContent = percentText(memory.used_percent);
  $("memory-detail").textContent = `${memory.used_mib ?? "—"} / ${memory.total_mib ?? "—"} MiB`;
  $("cpu-temperature").textContent = finite(temperature.celsius) ? `${temperature.celsius.toFixed(1)}°` : "—";
  $("temperature-source").textContent = temperature.source || "unavailable";
  $("system-uptime").textContent = uptimeText(payload.uptime_s);

  const services = payload.services || [];
  const serviceList = $("service-list");
  serviceList.innerHTML = "";
  let activeCount = 0;
  for (const service of services) {
    const ok = service.active === "active";
    if (ok) activeCount += 1;
    const row = document.createElement("div");
    row.className = `service-row ${ok ? "ok" : "bad"}`;
    const dot = document.createElement("i");
    dot.className = "health-dot";
    const name = document.createElement("strong");
    name.textContent = service.name;
    const role = document.createElement("span");
    role.textContent = service.role;
    const status = document.createElement("code");
    const started = finite(service.started_at_boot_s) ? ` · +${service.started_at_boot_s.toFixed(1)}s` : "";
    const restarts = service.restarts ? ` · restart ${service.restarts}` : "";
    status.textContent = `${service.active}/${service.sub}${started}${restarts}`;
    row.append(dot, name, role, status);
    serviceList.append(row);
  }
  $("service-summary").textContent = `${activeCount}/${services.length || 0} ACTIVE`;

  const stateNames = { R: "运行", S: "休眠", D: "I/O等待", T: "停止", Z: "僵尸" };
  const processBody = $("process-body");
  processBody.innerHTML = "";
  for (const process of payload.processes || []) {
    const row = document.createElement("tr");
    const missing = !process.pid;
    if (missing) row.className = "missing";
    const values = [
      process.name,
      process.pid ?? "—",
      missing ? "未运行" : (stateNames[process.state] || process.state),
      missing ? "—" : `${Number(process.cpu_percent || 0).toFixed(1)}%`,
      missing ? "—" : `${Number(process.rss_mib || 0).toFixed(1)} MiB`,
    ];
    for (const value of values) {
      const cell = document.createElement("td");
      cell.textContent = value;
      row.append(cell);
    }
    processBody.append(row);
  }

  const devices = payload.devices || {};
  setHealthValue("remoteproc-state", payload.remoteproc === "running", String(payload.remoteproc || "unknown").toUpperCase());
  setHealthValue("device-rpmsg", devices.rpmsg);
  setHealthValue("device-radar", devices.radar_uart);
  setHealthValue("device-ble", devices.ble_uart);
  setHealthValue("device-camera", devices.camera);
  setHealthValue("storage-state", storage.mounted, storage.mounted ? percentText(storage.used_percent) : "UNMOUNTED");
  $("storage-detail").textContent = storage.mounted
    ? `${storage.used_gib ?? "—"}/${storage.total_gib ?? "—"} GiB`
    : "storage unavailable";
  $("system-updated").textContent = `系统采样 ${new Date(payload.timestamp_ms).toLocaleTimeString("zh-CN", { hour12: false })} · ${payload.refresh_interval_s || 5}s`;
}

function renderBoot(payload) {
  $("boot-summary").textContent = payload.systemd_time || "systemd 启动时间不可用";
  $("boot-id").textContent = payload.boot_id || "—";
  const milestones = $("milestone-list");
  milestones.innerHTML = "";
  for (const item of payload.milestones || []) {
    const card = document.createElement("article");
    card.className = `milestone-card${item.ready ? " ready" : ""}`;
    const label = document.createElement("span");
    label.textContent = item.label;
    const value = document.createElement("strong");
    value.textContent = item.ready ? Number(item.time_s).toFixed(3) : "—";
    if (item.ready) {
      const unit = document.createElement("small");
      unit.textContent = " s";
      value.append(unit);
    }
    card.append(label, value);
    milestones.append(card);
  }

  const runtimeEvents = $("runtime-event-list");
  runtimeEvents.innerHTML = "";
  const critical = (payload.runtime_events || []).slice(0, 6);
  if (!critical.length) {
    runtimeEvents.innerHTML = '<p class="timeline-empty">本次启动后暂无关键业务事件</p>';
  } else {
    for (const event of critical) {
      const row = document.createElement("article");
      row.className = `runtime-event-row source-${event.source}`;
      const timing = document.createElement("time");
      const wall = document.createElement("strong");
      wall.textContent = new Date(event.timestamp_ms).toLocaleTimeString("zh-CN", { hour12: false });
      const sinceBoot = document.createElement("small");
      sinceBoot.textContent = `开机后 +${Number(event.since_boot_s).toFixed(3)} s`;
      timing.append(wall, sinceBoot);
      const content = document.createElement("div");
      const source = document.createElement("span");
      source.textContent = sourceName(event.source);
      const summary = document.createElement("strong");
      summary.textContent = eventSummary(event);
      const detail = document.createElement("small");
      detail.textContent = event.details || `event=${event.event_type}`;
      content.append(source, summary, detail);
      row.append(timing, content);
      runtimeEvents.append(row);
    }
  }

  const logs = $("boot-log-list");
  logs.innerHTML = "";
  const selected = (payload.logs || []).slice(0, 6);
  if (!selected.length) {
    logs.innerHTML = '<p class="timeline-empty">本次启动暂无关键日志</p>';
  } else {
    for (const entry of selected) {
      const row = document.createElement("div");
      const warning = /error|failed|warning|错误|失败|警告/i.test(entry.message);
      row.className = `boot-log-row${warning ? " warn" : ""}`;
      const time = document.createElement("time");
      time.textContent = `+${Number(entry.time_s).toFixed(3)}s`;
      const message = document.createElement("code");
      message.textContent = entry.message;
      message.title = entry.message;
      row.append(time, message);
      logs.append(row);
    }
  }
  $("boot-updated").textContent = `关键事件 ${payload.event_refresh_interval_s || 10}s · 启动信息缓存 ${payload.refresh_interval_s || 60}s`;
}

function stateLabel(task) {
  if (task.active === "active") return "运行中";
  if (task.active === "inactive") return "已暂停";
  if (task.active === "failed") return "异常";
  return `${task.active || "unknown"} / ${task.sub || "unknown"}`;
}

function requestControl(task, action) {
  const dialog = $("control-dialog");
  pendingControl = { task: task.key, action, name: task.name };
  const running = action === "run";
  $("control-dialog-title").textContent = `${running ? "运行" : "暂停"}${task.name}？`;
  $("control-dialog-message").textContent = task.key === "dvr" && !running
    ? "融合业务暂停后，雷达、摄像头、NPU 与 HUD 将停止；控制面板、M33 和网络保持在线。"
    : `${task.name}将执行 systemd ${running ? "start" : "stop"}，操作会写入控制审计。`;
  $("control-dialog-confirm").textContent = running ? "确认运行" : "确认暂停";
  $("control-dialog-confirm").classList.toggle("danger", !running);
  if (typeof dialog.showModal === "function") {
    dialog.showModal();
  } else if (window.confirm($("control-dialog-message").textContent)) {
    performControl();
  }
}

const maintenanceCopy = {
  tf_mount: {
    title: "识别并挂载 TF 卡？",
    message: "仅挂载已经插入的 /dev/mmcblk0p1。录像主存储位于板载 ext4，不依赖 TF 卡。",
    confirm: "确认挂载",
  },
  tf_eject: {
    title: "安全弹出 TF 卡？",
    message: "系统会先同步缓存再卸载 TF。看到成功提示后才能物理拔卡。",
    confirm: "同步并弹出",
  },
  project_stop: {
    title: "安全停止项目？",
    message: "2 秒后停止 DVR 和 Dashboard，并同步存储；M33、网络和 OTA 保持运行。页面随后会断开。",
    confirm: "停止项目",
  },
  system_poweroff: {
    title: "安全关闭开发板？",
    message: "2 秒后停止业务、同步全部存储并执行系统关机。请等待板卡完成关机后再断电。",
    confirm: "安全关机",
  },
};

function requestMaintenance(action) {
  const copy = maintenanceCopy[action];
  if (!copy) return;
  pendingControl = { scope: "maintenance", action, name: copy.title.replace("？", "") };
  $("control-dialog-title").textContent = copy.title;
  $("control-dialog-message").textContent = copy.message;
  $("control-dialog-confirm").textContent = copy.confirm;
  $("control-dialog-confirm").classList.toggle("danger", action === "project_stop" || action === "system_poweroff");
  const dialog = $("control-dialog");
  if (typeof dialog.showModal === "function") {
    dialog.showModal();
  } else if (window.confirm(copy.message)) {
    performControl();
  }
}

function renderControl(payload) {
  controlToken = payload.control_token || "";
  const guard = $("control-plane-state");
  guard.className = `guard-state ${payload.controls_enabled ? "online" : "blocked"}`;
  guard.textContent = payload.controls_enabled
    ? "独立面板服务在线 · 控制已解锁"
    : "独立面板服务未就绪 · 控制已锁定";

  const maintenance = payload.maintenance || {};
  const tfState = $("tf-card-state");
  if (maintenance.tf_mounted) {
    tfState.textContent = `TF 已挂载 · ${maintenance.tf_mount}`;
    tfState.className = "mounted";
  } else if (maintenance.tf_inserted) {
    tfState.textContent = "TF 已识别 · 尚未挂载";
    tfState.className = "inserted";
  } else {
    tfState.textContent = "未检测到 TF 卡";
    tfState.className = "missing";
  }
  $("recording-storage").textContent = `录像：${maintenance.recording_storage || "/usr/local/helmet/dvr"}`;
  document.querySelectorAll("[data-maintenance]").forEach((button) => {
    button.disabled = !payload.controls_enabled;
  });

  const tasks = $("controllable-task-list");
  tasks.innerHTML = "";
  for (const task of payload.tasks || []) {
    const row = document.createElement("article");
    const running = task.active === "active";
    row.className = `control-task ${running ? "running" : task.active === "failed" ? "failed" : "paused"}`;
    const indicator = document.createElement("i");
    const copy = document.createElement("div");
    const name = document.createElement("strong");
    name.textContent = task.name;
    const role = document.createElement("span");
    role.textContent = task.role;
    const unit = document.createElement("code");
    unit.textContent = `${task.unit} · ${stateLabel(task)}${task.pid ? ` · PID ${task.pid}` : ""}`;
    copy.append(name, role, unit);
    const button = document.createElement("button");
    const action = running ? "pause" : "run";
    button.className = `task-control-button ${running ? "pause" : "run"}`;
    button.textContent = running ? "暂停任务" : "运行任务";
    button.disabled = !payload.controls_enabled;
    button.addEventListener("click", () => requestControl(task, action));
    row.append(indicator, copy, button);
    tasks.append(row);
  }

  const protectedTasks = $("protected-task-list");
  protectedTasks.innerHTML = "";
  for (const task of payload.protected || []) {
    const row = document.createElement("article");
    row.className = `protected-task ${task.active === "active" ? "online" : "offline"}`;
    const dot = document.createElement("i");
    const copy = document.createElement("div");
    const name = document.createElement("strong");
    name.textContent = task.name;
    const role = document.createElement("span");
    role.textContent = task.role;
    copy.append(name, role);
    const state = document.createElement("code");
    state.textContent = stateLabel(task);
    row.append(dot, copy, state);
    protectedTasks.append(row);
  }

  const audit = $("control-audit-list");
  audit.innerHTML = "";
  if (!(payload.audit || []).length) {
    audit.innerHTML = '<p class="timeline-empty">尚无控制操作</p>';
  } else {
    for (const entry of payload.audit.slice(0, 4)) {
      const row = document.createElement("article");
      row.className = `control-audit-row ${entry.result === "ok" ? "ok" : "failed"}`;
      const time = document.createElement("time");
      time.textContent = new Date(Number(entry.timestamp_ms)).toLocaleTimeString("zh-CN", { hour12: false });
      const detail = document.createElement("span");
      const actionNames = { pause: "暂停", run: "运行", mount: "挂载", eject: "弹出", stop: "停止", poweroff: "关机" };
      detail.textContent = `${entry.task_name} · ${actionNames[entry.action] || entry.action}`;
      const result = document.createElement("strong");
      result.textContent = entry.result === "ok" ? "成功" : "失败";
      row.append(time, detail, result);
      audit.append(row);
    }
  }
  $("control-updated").textContent = `状态采样 ${new Date(payload.timestamp_ms).toLocaleTimeString("zh-CN", { hour12: false })}`;
}

async function performControl() {
  const request = pendingControl;
  pendingControl = null;
  if (!request) return;
  try {
    const maintenance = request.scope === "maintenance";
    const response = await fetch(maintenance ? "/api/maintenance" : "/api/control", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(maintenance ? {
        action: request.action,
        confirmation: `maintenance:${request.action}`,
        control_token: controlToken,
      } : {
        task: request.task,
        action: request.action,
        confirmation: `${request.task}:${request.action}`,
        control_token: controlToken,
      }),
    });
    const payload = await response.json();
    if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`);
    showToast(maintenance ? `${request.name}已调度` : `${request.name}${request.action === "pause" ? "已暂停" : "已运行"}`);
    await pollControl();
    pollSystem();
    pollBoot();
  } catch (error) {
    showToast(`控制失败：${error.message}`, true);
    pollControl();
  }
}

function bindControlDialog() {
  $("control-dialog-confirm").addEventListener("click", (event) => {
    event.preventDefault();
    $("control-dialog").close("confirm");
    performControl();
  });
  $("control-dialog").addEventListener("close", () => {
    if ($("control-dialog").returnValue === "cancel") pendingControl = null;
  });
  document.querySelectorAll("[data-maintenance]").forEach((button) => {
    button.addEventListener("click", () => requestMaintenance(button.dataset.maintenance));
  });
}

async function pollEvents() {
  if (eventsPolling || document.hidden) return;
  eventsPolling = true;
  try {
    const response = await fetch("/api/events?limit=160", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderSensorEvents(await response.json());
  } catch (_) {
    $("camera-state").textContent = "同步接口断开";
  } finally {
    eventsPolling = false;
  }
}

async function pollState() {
  if (statePolling || document.hidden) return;
  statePolling = true;
  try {
    const response = await fetch("/api/state", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderState(await response.json());
  } catch (_) {
    const chip = $("connection-chip");
    chip.className = "status-chip status-offline";
    chip.querySelector("span").textContent = "Dashboard 连接中断";
  } finally {
    statePolling = false;
  }
}

async function pollSystem() {
  if (systemPolling || document.hidden) return;
  systemPolling = true;
  try {
    const response = await fetch("/api/system", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderSystem(await response.json());
  } catch (_) {
    $("service-summary").textContent = "SYSTEM API OFFLINE";
  } finally {
    systemPolling = false;
  }
}

async function pollBoot() {
  if (bootPolling || document.hidden) return;
  bootPolling = true;
  try {
    const response = await fetch("/api/boot", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderBoot(await response.json());
  } catch (_) {
    $("boot-summary").textContent = "关键时间接口暂不可用";
  } finally {
    bootPolling = false;
  }
}

async function pollControl() {
  if (controlPolling || document.hidden) return;
  controlPolling = true;
  try {
    const response = await fetch("/api/control", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    renderControl(await response.json());
  } catch (_) {
    const guard = $("control-plane-state");
    guard.className = "guard-state blocked";
    guard.textContent = "控制接口不可用";
  } finally {
    controlPolling = false;
  }
}

bindTabs();
bindControlDialog();
renderEvents();
pollState();
window.setInterval(pollState, 500);
window.setInterval(() => {
  if (activeView === "sensors") pollEvents();
}, 2000);
window.setInterval(() => {
  if (activeView === "system") pollSystem();
}, 5000);
window.setInterval(() => {
  if (activeView === "boot") pollBoot();
}, 10000);
window.setInterval(() => {
  if (activeView === "control") pollControl();
}, 5000);
window.setInterval(updateEventTimers, 500);
window.addEventListener("resize", () => {
  if (activeView === "radar") charts.forEach(drawChart);
});
document.addEventListener("visibilitychange", () => {
  if (!document.hidden) {
    pollState();
    if (activeView === "sensors") pollEvents();
    if (activeView === "system") pollSystem();
    if (activeView === "boot") pollBoot();
    if (activeView === "control") pollControl();
  }
});
