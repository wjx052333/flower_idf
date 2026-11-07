#!/usr/bin/env python3
"""
mock_device.py — ESPWROM32-4M-8xMOS 固件对外接口模拟器（仅标准库）

模拟的接口（行为对齐 ../ESPWROM32-4M-8xMOS/main/main.c、water_logic.c）：
  HTTP
    GET /                              固件自带控制面板（启动时从 main.c 的 STATUS_HTML 提取）
    GET /change_relay1..8[?duration=ms | ?action=off]
    GET /api/sensors
    GET /api/water/config[?strategy=..&surface_thresh_pct=..&...]
  UDP 设备发现（端口 3333）
    每 5 秒广播 "FLOWER:mos_{id}:{ip}:{port}"，收到 "FLOWER_DISCOVER" 单播回复

模拟器专用（固件里没有）：
  GET  /debug                        调试页：注入传感器数据、开关箱门、按板载按键
  GET  /debug/api/state              当前注入值
  POST /debug/api/inject             JSON，部分字段即可；注入值一直保持，并写入状态文件
  POST /debug/api/button             模拟板载按键（切换 MOS1）

安全逻辑同固件：UVB 门磁互锁 + 开门断电 + 爆闪报警，加热垫温控锁，单次最长时长裁剪，
到时自动关闭，自动浇水策略每 30 秒检查一次（UTC+8）。

Usage:
  python mock_device.py [--port 8080] [--device-id mock001] [--no-discovery]
"""

import argparse
import json
import os
import re
import socket
import threading
import time
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FW_SRC = os.path.join(HERE, "..", "ESPWROM32-4M-8xMOS", "main", "main.c")

# ── Firmware constants (main.c) ──────────────────────────────────────────────
FW_VERSION = "1.4.0"
NUM_RELAYS = 8
CH_PUMP, CH_UVB, CH_STROBE, CH_HEATER = 0, 1, 2, 3
RELAY_GPIO = [19, 18, 17, 16, 33, 25, 26, 27]
RELAY_MAX_MS = [180000, 180000, 30000, 3600000, 3600000, 60000, 60000, 60000]
RELAY_DEVICE = ["pump", "uvb", "strobe", "heater", "fan", "mist", "spare7", "spare8"]
RELAY_NAME_CN = ["水泵", "紫外灯", "爆闪灯", "加热垫", "风扇", "喷雾", "7号备用通道", "8号备用通道"]
RELAY_VERB_CN = ["抽", "照", "闪", "开", "吹", "喷", "开", "开"]
RELAY_DEFAULT_DURATION_MS = 10000
STROBE_ALARM_MS = 1000
HEATER_SOIL_MAX_C = 32.0
HEATER_AIR_MAX_C = 35.0
UINT32_MAX = 0xFFFFFFFF

APP_TICK_S = 0.05          # relay auto-off
SENSOR_TICK_S = 2.0        # heater lock
STATUS_TICK_S = 30.0       # auto watering
REED_COOLDOWN_S = 1.5

# ── Water logic constants (water_logic.h / .c) ──────────────────────────────
WATER_CHECK_START_HOUR, WATER_CHECK_END_HOUR = 8, 16
WATER_COOLDOWN_S = 1800
DEF_RAW_DRY, DEF_RAW_WET = 3110, 958
STRATEGY_NAMES = ["中午12点", "浇表面", "干了再浇透", "关闭"]
WATER_STRATEGY_NOON, WATER_STRATEGY_SURFACE, WATER_STRATEGY_DEEP, WATER_STRATEGY_OFF = range(4)
DEFAULT_WATER_CFG = {
    "strategy": WATER_STRATEGY_OFF,
    "surface_thresh_pct": 50, "surface_dur_s": 60,
    "deep_thresh_pct": 10, "deep_dur_s": 1200,
    "raw_dry": DEF_RAW_DRY, "raw_wet": DEF_RAW_WET,
}
CST = timezone(timedelta(hours=8))

DISCOVERY_PORT = 3333
DISCOVERY_PROBE_CMD = b"FLOWER_DISCOVER"
DISCOVERY_INTERVAL_S = 5

DEFAULT_SENSORS = {
    "raw_low": 2000,        # ADC counts, GPIO34
    "soil_ok": True,        # DS18B20 readable
    "soil_c": 25.0,
    "lux_ok": True,         # BH1750 readable
    "lux": 300.0,
    "aht_ok": True,
    "air_c": 26.0,
    "air_humi": 60.0,
    "door_closed": True,    # reed switch level 0
}


def water_calc_humidity(raw, raw_dry, raw_wet):
    if raw_dry <= raw_wet:
        raw_dry, raw_wet = DEF_RAW_DRY, DEF_RAW_WET
    hum = (raw_dry - raw) * 100.0 / (raw_dry - raw_wet)
    return min(100.0, max(0.0, hum))


class Relay:
    def __init__(self, gpio):
        self.gpio = gpio
        self.on = False
        self.on_t = 0.0                 # monotonic seconds
        self.duration_ms = RELAY_DEFAULT_DURATION_MS
        self.last_on_utc_s = 0
        self.last_on_dur_ms = 0


class Device:
    """Device state; every public method takes self.lock."""

    def __init__(self, state_file):
        self.lock = threading.RLock()
        self.state_file = state_file
        self.relays = [Relay(g) for g in RELAY_GPIO]
        self.last_requested_ms = [0] * NUM_RELAYS
        self.sensors = dict(DEFAULT_SENSORS)
        self.water = dict(DEFAULT_WATER_CFG)
        self.noon_done_today = False
        self.last_auto_water = 0
        self.reed_isr_count = 0
        self.reed_open_count = 0
        self.reed_last_open_ts = 0
        self.reed_last_trigger_t = -1e9
        self.uvb_cut_by_door_ts = 0
        self.uvb_cut_lit_ms = 0
        self.heater_lock = True
        self._load()
        self.heater_lock = self._heater_check() is not None

    # ── persistence (water config ≈ NVS, injected sensors held across restarts)
    def _load(self):
        try:
            with open(self.state_file, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            return
        for k in self.sensors:
            if k in data.get("sensors", {}):
                self.sensors[k] = data["sensors"][k]
        for k in self.water:
            if k in data.get("water", {}):
                self.water[k] = int(data["water"][k])

    def _save(self):
        tmp = self.state_file + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"sensors": self.sensors, "water": self.water}, f,
                      ensure_ascii=False, indent=2)
        os.replace(tmp, self.state_file)

    # ── sensor cache view
    def humidity(self):
        return water_calc_humidity(self.sensors["raw_low"],
                                   self.water["raw_dry"], self.water["raw_wet"])

    def _heater_check(self):
        s = self.sensors
        if (s["soil_ok"] and s["soil_c"] > HEATER_SOIL_MAX_C) or \
           (s["aht_ok"] and s["air_c"] > HEATER_AIR_MAX_C):
            return "over_temp"
        if not s["soil_ok"] and not s["aht_ok"]:
            return "no_temp"
        return None

    # ── relays (relay_set_unlocked)
    def _relay_set(self, ch, on, duration_ms):
        """Returns None on success, else the refusal reason."""
        if on:
            if ch == CH_UVB and not self.sensors["door_closed"]:
                self._strobe_alarm()
                return "door_open"
            if ch == CH_HEATER:
                why = self._heater_check()
                if why:
                    return why
            self.last_requested_ms[ch] = duration_ms
            if duration_ms == 0 or duration_ms > RELAY_MAX_MS[ch]:
                duration_ms = RELAY_MAX_MS[ch]
            r = self.relays[ch]
            r.on_t = time.monotonic()
            r.duration_ms = duration_ms
            r.last_on_utc_s = int(time.time())
            r.last_on_dur_ms = duration_ms
        self.relays[ch].on = on
        return None

    def _strobe_alarm(self):
        if not self.relays[CH_STROBE].on:
            self._relay_set(CH_STROBE, True, STROBE_ALARM_MS)

    def _uvb_door_cut(self, cut_t):
        r = self.relays[CH_UVB]
        self.uvb_cut_lit_ms = max(0, int((cut_t - r.on_t) * 1000))
        self.uvb_cut_by_door_ts = int(time.time())
        self._relay_set(CH_UVB, False, 0)
        print(f"[SAFETY CUT] door opened while UVB was on (lit {self.uvb_cut_lit_ms} ms)")
        self._strobe_alarm()

    def relay_on(self, ch, dur):
        with self.lock:
            if dur == 0:
                dur = self.relays[ch].duration_ms or RELAY_DEFAULT_DURATION_MS
            why = self._relay_set(ch, True, dur)
            return why, self.last_requested_ms[ch], self.relays[ch].duration_ms

    def relay_off(self, ch):
        with self.lock:
            self._relay_set(ch, False, 0)

    def button(self):
        with self.lock:
            r = self.relays[CH_PUMP]
            if r.on:
                self._relay_set(CH_PUMP, False, 0)
            else:
                self._relay_set(CH_PUMP, True, r.duration_ms or RELAY_DEFAULT_DURATION_MS)

    # ── door (reed_isr_handler + reed_task)
    def _set_door(self, closed):
        was_closed = self.sensors["door_closed"]
        self.sensors["door_closed"] = closed
        if closed or not was_closed:
            return                      # ISR is rising-edge only: closed -> open
        now = time.monotonic()
        self.reed_isr_count += 1
        if self.relays[CH_UVB].on:
            self._uvb_door_cut(now)
        if now - self.reed_last_trigger_t < REED_COOLDOWN_S:
            return
        self.reed_last_trigger_t = now
        self.reed_open_count += 1
        self.reed_last_open_ts = int(time.time())
        print(f"Reed switch opened (count={self.reed_open_count})")

    def inject(self, data):
        """Apply a partial sensor update. Raises ValueError on bad input."""
        with self.lock:
            nxt = dict(self.sensors)
            for k, v in data.items():
                if k not in DEFAULT_SENSORS:
                    raise ValueError(f"{k}: unknown field")
                if isinstance(DEFAULT_SENSORS[k], bool):
                    if not isinstance(v, bool):
                        raise ValueError(f"{k}: boolean required")
                elif k == "raw_low":
                    if not isinstance(v, int) or isinstance(v, bool) or not 0 <= v <= 4095:
                        raise ValueError("raw_low: integer 0-4095 required")
                else:
                    if not isinstance(v, (int, float)) or isinstance(v, bool):
                        raise ValueError(f"{k}: number required")
                    v = float(v)
                nxt[k] = v
            door = nxt.pop("door_closed")
            nxt["door_closed"] = self.sensors["door_closed"]
            self.sensors = nxt
            self._set_door(door)
            self._save()
            return self.state()

    def state(self):
        with self.lock:
            return {"sensors": dict(self.sensors),
                    "humidity": round(self.humidity(), 1),
                    "raw_dry": self.water["raw_dry"], "raw_wet": self.water["raw_wet"]}

    # ── water config (water_config_apply_query)
    def water_apply_query(self, query):
        if len(query) >= 192:
            return "query too long"
        with self.lock:
            nxt = dict(self.water)
            for pair in query.split("&"):
                if not pair:
                    continue
                if "=" not in pair:
                    return "bad parameter"
                key, val = pair.split("=", 1)
                if not re.fullmatch(r"\s*[+-]?\d+", val):
                    return f"{key}: not a number"
                v = int(val)
                if key == "strategy":
                    if not 0 <= v < len(STRATEGY_NAMES):
                        return f"strategy: 0-{len(STRATEGY_NAMES) - 1} required"
                elif key in ("surface_thresh_pct", "deep_thresh_pct"):
                    if not 0 <= v <= 100:
                        return f"{key}: 0-100 required"
                elif key in ("surface_dur_s", "deep_dur_s"):
                    if not 1 <= v <= 65535:
                        return f"{key}: 1-65535 required"
                elif key in ("raw_dry", "raw_wet"):
                    if not 0 <= v <= 4095:
                        return f"{key}: 0-4095 required"
                else:
                    return f"{key}: unknown field"
                nxt[key] = v
            if nxt["raw_dry"] <= nxt["raw_wet"]:
                return "raw_dry must be greater than raw_wet"
            self.water = nxt
            self._save()
            return None

    def water_json(self):
        with self.lock:
            w = self.water
            return {
                "strategy": w["strategy"],
                "strategy_name": STRATEGY_NAMES[w["strategy"]],
                "surface_thresh_pct": w["surface_thresh_pct"], "surface_dur_s": w["surface_dur_s"],
                "deep_thresh_pct": w["deep_thresh_pct"], "deep_dur_s": w["deep_dur_s"],
                "raw_dry": w["raw_dry"], "raw_wet": w["raw_wet"],
                "check_window": f"{WATER_CHECK_START_HOUR:02d}:00-{WATER_CHECK_END_HOUR:02d}:00",
                "cooldown_s": WATER_COOLDOWN_S,
            }

    # ── /api/sensors
    def sensors_json(self):
        with self.lock:
            s = self.sensors
            now = time.monotonic()
            soil_c = round(s["soil_c"], 2) if s["soil_ok"] else -273.0
            lux = round(s["lux"], 1) if s["lux_ok"] and s["lux"] >= 0 else -1.0
            air_c = round(s["air_c"], 2) if s["aht_ok"] else -99.0
            air_humi = round(s["air_humi"], 1) if s["aht_ok"] else -99.0
            hum = round(self.humidity(), 1)
            reed = 1 if s["door_closed"] else 0
            pump = self.relays[CH_PUMP]
            relays = []
            for i, r in enumerate(self.relays):
                remaining = 0
                if r.on:
                    remaining = max(0, int(r.duration_ms - (now - r.on_t) * 1000))
                relays.append({
                    "ch": i + 1, "device": RELAY_DEVICE[i], "gpio": r.gpio,
                    "state": 1 if r.on else 0, "remaining_ms": remaining,
                    "max_ms": RELAY_MAX_MS[i], "last_on_ts": r.last_on_utc_s,
                    "last_on_dur_ms": r.last_on_dur_ms,
                })
            return {
                "version": FW_VERSION,
                "humidity": hum, "raw_low": s["raw_low"], "temperature": soil_c,
                "lux": lux, "reed": reed,
                "reed_isr_cnt": self.reed_isr_count, "reed_open_cnt": self.reed_open_count,
                "reed_last_ts": self.reed_last_open_ts,
                "aht_temp": air_c, "aht_humi": air_humi,
                "relay_last_on_ts": pump.last_on_utc_s,
                "relay_last_on_dur_ms": pump.last_on_dur_ms,
                "strategy": self.water["strategy"],
                "soil_moisture": hum, "soil_temp": soil_c, "air_temp": air_c, "air_humi": air_humi,
                "door_closed": bool(reed),
                "uvb_cut_by_door_ts": self.uvb_cut_by_door_ts,
                "uvb_cut_lit_ms": self.uvb_cut_lit_ms,
                "heater_lock": self.heater_lock,
                "relays": relays,
            }

    # ── periodic work
    def app_tick(self):
        with self.lock:
            if self.relays[CH_UVB].on and not self.sensors["door_closed"]:
                self._uvb_door_cut(time.monotonic())
            now = time.monotonic()
            for ch, r in enumerate(self.relays):
                if r.on and r.duration_ms > 0 and (now - r.on_t) * 1000 >= r.duration_ms:
                    self._relay_set(ch, False, 0)

    def sensor_tick(self):
        with self.lock:
            why = self._heater_check()
            self.heater_lock = why is not None
            if why and self.relays[CH_HEATER].on:
                self._relay_set(CH_HEATER, False, 0)
                print(f"[SAFETY CUT] heater {why}")

    def status_tick(self):
        """check_auto_water(): water_strategy_tick + water_strategy_eval."""
        with self.lock:
            now = int(time.time())
            t = datetime.fromtimestamp(now, CST)
            if t.hour == 0 and t.minute == 0:
                self.noon_done_today = False
            w = self.water
            if w["strategy"] == WATER_STRATEGY_OFF:
                return
            hum = self.humidity()
            dur_ms = 0
            if w["strategy"] == WATER_STRATEGY_NOON:
                if t.hour != 12 or t.minute != 0 or self.noon_done_today:
                    return
                self.noon_done_today = True
                self.last_auto_water = now
                dur_ms = (360000 if hum > 80 else 480000 if hum > 70 else
                          600000 if hum > 60 else 900000 if hum > 50 else 1200000)
            else:
                if not WATER_CHECK_START_HOUR <= t.hour < WATER_CHECK_END_HOUR:
                    return
                if now - self.last_auto_water < WATER_COOLDOWN_S:
                    return
                surface = w["strategy"] == WATER_STRATEGY_SURFACE
                thresh = w["surface_thresh_pct"] if surface else w["deep_thresh_pct"]
                if hum >= thresh:
                    return
                self.last_auto_water = now
                dur_ms = (w["surface_dur_s"] if surface else w["deep_dur_s"]) * 1000
            if self.relays[CH_PUMP].on:
                print(f"Auto watering: relay 1 already on, skipping {dur_ms} ms")
                return
            print(f"Auto watering [{STRATEGY_NAMES[w['strategy']]}]: humidity={hum:.1f}%, "
                  f"opening relay 1 for {dur_ms} ms")
            self._relay_set(CH_PUMP, True, dur_ms)


def every(period, fn):
    def loop():
        nxt = time.monotonic()
        while True:
            nxt += period
            try:
                fn()
            except Exception as e:  # keep the timer alive like an esp_timer would
                print(f"timer error: {e!r}")
            time.sleep(max(0.0, nxt - time.monotonic()))
    threading.Thread(target=loop, daemon=True).start()


# ── HTTP ─────────────────────────────────────────────────────────────────────
def load_status_html(path):
    """Extract STATUS_HTML from main.c so / serves exactly the firmware page."""
    with open(path, encoding="utf-8") as f:
        src = f.read()
    m = re.search(r"static const char STATUS_HTML\[\]\s*=(.*?);\s*\n", src, re.S)
    if not m:
        raise ValueError("STATUS_HTML not found")
    escapes = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}
    parts = re.findall(r'"((?:[^"\\\n]|\\.)*)"', m.group(1))
    return "".join(re.sub(r"\\(.)", lambda e: escapes.get(e.group(1), e.group(1)), p)
                   for p in parts)


def parse_duration_ms(s):
    if not s or not s.isdigit() or not s.isascii():
        return None
    return min(int(s), UINT32_MAX)


def query_value(query, key):
    """httpd_query_key_value(): first match, no URL decoding, 24-byte buffer.
    Returns (status, value) with status 'ok' | 'missing' | 'trunc'."""
    for pair in query.split("&"):
        k, sep, v = pair.partition("=")
        if k == key and sep:
            return ("trunc", None) if len(v) >= 24 else ("ok", v)
    return "missing", None


def json_temp(ok, v):
    return round(v, 1) if ok else None


class Handler(BaseHTTPRequestHandler):
    server_version = "mock-esp32-8xmos"
    dev: Device = None
    status_html = ""
    debug_html = ""

    def log_message(self, fmt, *args):
        if not self.path.startswith("/api/sensors"):
            super().log_message(fmt, *args)

    def send_body(self, code, ctype, body):
        data = body.encode("utf-8") if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(data)

    def send_json(self, obj, code=200):
        self.send_body(code, "application/json; charset=utf-8",
                       json.dumps(obj, ensure_ascii=False, separators=(",", ":")))

    def send_text(self, code, text):
        self.send_body(code, "text/plain; charset=utf-8", text)

    def do_GET(self):
        u = urlsplit(self.path)
        path, query = u.path, u.query
        m = re.fullmatch(r"/change_relay([1-8])", path)
        if path == "/":
            self.send_body(200, "text/html; charset=utf-8", self.status_html)
        elif m:
            self.change_relay(int(m.group(1)) - 1, query)
        elif path == "/api/sensors":
            self.send_json(self.dev.sensors_json())
        elif path == "/api/water/config":
            self.water_config(query)
        elif path == "/debug":
            self.send_body(200, "text/html; charset=utf-8", self.debug_html)
        elif path == "/debug/api/state":
            self.send_json(self.dev.state())
        else:
            self.send_text(404, "This URI does not exist")

    def do_POST(self):
        path = urlsplit(self.path).path
        if path == "/debug/api/inject":
            try:
                n = int(self.headers.get("Content-Length") or 0)
                data = json.loads(self.rfile.read(n) or b"{}")
                if not isinstance(data, dict):
                    raise ValueError("JSON object required")
                self.send_json(self.dev.inject(data))
            except ValueError as e:
                self.send_json({"error": str(e)}, 400)
        elif path == "/debug/api/button":
            self.dev.button()
            self.send_json({"ok": True})
        else:
            self.send_text(404, "This URI does not exist")

    def change_relay(self, ch, query):
        dev = self.dev
        base = {"ok": False, "ch": ch + 1, "device": RELAY_DEVICE[ch]}
        if len(query) >= 128:
            return self.send_json({**base, "reason": "bad_request",
                                   "message": "请求参数太长"}, 400)
        st, val = query_value(query, "action")
        if st == "ok" and val == "off":
            dev.relay_off(ch)
            return self.send_json({"ok": True, "ch": ch + 1, "device": RELAY_DEVICE[ch],
                                   "action": "off"})
        dur = 0
        st, val = query_value(query, "duration")
        if st == "ok":
            dur = parse_duration_ms(val)
        if st == "trunc" or dur is None:
            return self.send_json({**base, "reason": "bad_duration",
                                   "message": "时长参数不对，要用毫秒整数"}, 400)

        why, requested_ms, actual_ms = dev.relay_on(ch, dur)
        if why:
            with dev.lock:
                s = dict(dev.sensors)
            name = RELAY_NAME_CN[ch]
            if why == "door_open":
                msg = f"箱门开着，控制板拒绝开{name}"
            elif why == "over_temp":
                if s["soil_ok"] and s["soil_c"] > HEATER_SOIL_MAX_C:
                    msg = f"土温超过{HEATER_SOIL_MAX_C:.0f}度，控制板拒绝开{name}"
                else:
                    msg = f"箱温超过{HEATER_AIR_MAX_C:.0f}度，控制板拒绝开{name}"
            else:
                msg = f"读不到土温和箱温，控制板拒绝开{name}"
            body = {**base, "reason": why}
            if ch == CH_HEATER:
                body["soil_c"] = json_temp(s["soil_ok"], s["soil_c"])
                body["air_c"] = json_temp(s["aht_ok"], s["air_c"])
            body["message"] = msg
            return self.send_json(body, 403)

        body = {"ok": True, "ch": ch + 1, "device": RELAY_DEVICE[ch],
                "requested_ms": requested_ms, "actual_ms": actual_ms,
                "clamped": actual_ms != requested_ms}
        if body["clamped"]:
            body["message"] = (f"{RELAY_NAME_CN[ch]}一次最多{RELAY_VERB_CN[ch]}"
                               f"{actual_ms // 1000}秒")
        self.send_json(body)

    def water_config(self, query):
        if len(query) >= 256:
            return self.send_text(400, "query too long")
        if query:
            err = self.dev.water_apply_query(query)
            if err:
                print(f"Water config rejected: {err}")
                return self.send_text(400, err)
            print(f"Water config updated: {query}")
        self.send_json(self.dev.water_json())


# ── UDP device discovery (components/device_discovery) ───────────────────────
def local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))   # no packet is sent
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def discovery_loop(hostname, http_port, advertise_ip):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(1.0)
    sock.bind(("", DISCOVERY_PORT))
    print(f"Discovery: listening on UDP {DISCOVERY_PORT}, broadcasting every "
          f"{DISCOVERY_INTERVAL_S} s")
    last = -1e9
    while True:
        msg = f"FLOWER:{hostname}:{advertise_ip or local_ip()}:{http_port}".encode()
        now = time.monotonic()
        if now - last >= DISCOVERY_INTERVAL_S:
            try:
                sock.sendto(msg, ("255.255.255.255", DISCOVERY_PORT))
            except OSError as e:
                print(f"Discovery broadcast failed: {e}")
            last = now
        try:
            data, addr = sock.recvfrom(127)
        except socket.timeout:
            continue
        except OSError:          # e.g. Windows ICMP port unreachable → WSAECONNRESET
            continue
        if data.startswith(DISCOVERY_PROBE_CMD):
            print(f"Discovery: probe from {addr[0]}:{addr[1]}, replying")
            sock.sendto(msg, addr)


def main():
    ap = argparse.ArgumentParser(description="ESPWROM32-4M-8xMOS HTTP/UDP mock")
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080, help="HTTP port (device uses 80)")
    ap.add_argument("--device-id", default="mock001")
    ap.add_argument("--advertise-ip", help="IP put in discovery packets (default: auto)")
    ap.add_argument("--no-discovery", action="store_true")
    ap.add_argument("--firmware-src", default=DEFAULT_FW_SRC,
                    help="main.c to take the / page from")
    ap.add_argument("--state-file", default=os.path.join(HERE, "mock_state.json"))
    args = ap.parse_args()

    dev = Device(args.state_file)
    Handler.dev = dev
    try:
        Handler.status_html = load_status_html(args.firmware_src)
    except (OSError, ValueError) as e:
        print(f"Cannot load STATUS_HTML from {args.firmware_src}: {e}")
        Handler.status_html = (f"<meta charset=utf-8><p>无法从 {args.firmware_src} 提取 "
                               f"STATUS_HTML（{e}）。<a href=/debug>调试页</a></p>")
    with open(os.path.join(HERE, "debug.html"), encoding="utf-8") as f:
        Handler.debug_html = f.read()

    every(APP_TICK_S, dev.app_tick)
    every(SENSOR_TICK_S, dev.sensor_tick)
    every(STATUS_TICK_S, dev.status_tick)

    if not args.no_discovery:
        threading.Thread(target=discovery_loop, daemon=True,
                         args=(f"mos_{args.device_id}", args.port, args.advertise_ip)).start()

    srv = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"HTTP on http://{args.host}:{args.port}/  (debug: /debug), state: {args.state_file}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
