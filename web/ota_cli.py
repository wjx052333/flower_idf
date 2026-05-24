#!/usr/bin/env python3
"""
ota_cli.py — 命令行触发设备 OTA，无需打开浏览器。

Usage:
  python3 ota_cli.py <device_id> <firmware_file> [options]

  <device_id>      目标设备 ID，例如 flr001
  <firmware_file>  固件 .bin 文件路径

Options:
  --backend URL    后端地址（默认 https://localhost:8080）
  --user USER      用户名（默认 floweruser0）
  --pass PASS      密码（默认 Test1234!）
  --server-ip IP   固件服务器对设备可见的 IP（默认自动检测本机局域网 IP）
  --server-port N  固件服务器端口（默认 9101）
  --broker URL     MQTT broker（默认从 backend 地址推断，mqtts://HOST:8883）
  --insecure       跳过 TLS 证书校验（自签名证书时使用）
  --wait N         等待设备 OTA 响应的秒数（默认 120，0=不等待）
"""

import argparse
import http.server
import json
import os
import socket
import ssl
import sys
import threading
import time
import urllib.parse
import urllib.request

import paho.mqtt.client as mqtt
from paho.mqtt.properties import Properties
from paho.mqtt.packettypes import PacketTypes

from flower_pb2 import Command, CommandResponse


# ── HTTP firmware server ──────────────────────────────────────────────────────

class _FirmwareHandler(http.server.BaseHTTPRequestHandler):
    firmware_path: str = ""
    firmware_name: str = ""

    def do_GET(self):
        name = urllib.parse.unquote(os.path.basename(self.path))
        if name != self.firmware_name:
            self.send_error(404)
            return
        size = os.path.getsize(self.firmware_path)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        with open(self.firmware_path, "rb") as f:
            self.wfile.write(f.read())
        print(f"[FW]   served {name} ({size // 1024} KB)", flush=True)

    def log_message(self, fmt, *args):
        pass  # silence access log


def _start_fw_server(firmware_path: str, port: int) -> http.server.HTTPServer:
    name = os.path.basename(firmware_path)

    class Handler(_FirmwareHandler):
        pass

    Handler.firmware_path = firmware_path
    Handler.firmware_name = name

    server = http.server.HTTPServer(("0.0.0.0", port), Handler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    return server


# ── Local IP detection ────────────────────────────────────────────────────────

def _local_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


# ── Backend login ─────────────────────────────────────────────────────────────

def _login(backend: str, user: str, password: str, insecure: bool) -> str:
    ctx = ssl.create_default_context()
    if insecure:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE

    body = json.dumps({"username": user, "password": password}).encode()
    req = urllib.request.Request(
        f"{backend}/api/login",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, context=ctx, timeout=10) as resp:
        data = json.load(resp)

    token = data.get("access_token")
    if not token:
        raise RuntimeError(f"login failed: {data.get('error', data)}")
    return token


def _jwt_sub(token: str) -> str:
    import base64
    parts = token.split(".")
    if len(parts) < 2:
        return ""
    pad = parts[1] + "=" * (-len(parts[1]) % 4)
    try:
        return json.loads(base64.urlsafe_b64decode(pad)).get("sub", "")
    except Exception:
        return ""


# ── Main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="CLI OTA trigger for Flower devices"
    )
    parser.add_argument("device_id",    help="设备 ID，例如 flr001")
    parser.add_argument("firmware",     help="固件 .bin 文件路径")
    parser.add_argument("--backend",    default="https://localhost:8080")
    parser.add_argument("--user",       default="floweruser0")
    parser.add_argument("--pass",      dest="password", default="Test1234!")
    parser.add_argument("--server-ip",  default=None)
    parser.add_argument("--server-port",type=int, default=9101)
    parser.add_argument("--broker",     default=None,
                        help="MQTT broker URL，例如 mqtts://HOST:8883")
    parser.add_argument("--insecure",   action="store_true",
                        help="跳过 TLS 证书校验")
    parser.add_argument("--wait",       type=int, default=120,
                        help="等待 OTA 响应的秒数（0=不等待）")
    args = parser.parse_args()

    fw_path = os.path.abspath(args.firmware)
    if not os.path.isfile(fw_path):
        sys.exit(f"[ERROR] 固件文件不存在: {fw_path}")

    server_ip   = args.server_ip or _local_ip()
    server_port = args.server_port
    fw_name     = os.path.basename(fw_path)
    fw_url      = f"http://{server_ip}:{server_port}/{fw_name}"

    # 1. Start local firmware HTTP server
    _start_fw_server(fw_path, server_port)
    print(f"[FW]   serving {fw_name} at {fw_url}", flush=True)

    # 2. Login
    print(f"[AUTH] logging in as {args.user} …", flush=True)
    token   = _login(args.backend, args.user, args.password, args.insecure)
    user_sub = _jwt_sub(token) or args.user
    print(f"[AUTH] OK  sub={user_sub}", flush=True)

    # 3. Derive broker URL
    backend_host = urllib.parse.urlparse(args.backend).hostname
    broker_url   = args.broker or f"mqtts://{backend_host}:8883"
    use_tls      = broker_url.startswith("mqtts://") or broker_url.startswith("ssl://")
    host_port    = (broker_url
                    .removeprefix("mqtts://")
                    .removeprefix("ssl://")
                    .removeprefix("mqtt://"))
    host, port_str = (
        host_port.rsplit(":", 1) if ":" in host_port
        else (host_port, "8883" if use_tls else "1883")
    )
    broker_port = int(port_str)

    # 4. Build MQTT client
    try:
        client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"app_{user_sub}_{os.urandom(4).hex()}",
            protocol=mqtt.MQTTv5,
        )
    except AttributeError:
        client = mqtt.Client(
            client_id=f"app_{user_sub}_{os.urandom(4).hex()}",
            protocol=mqtt.MQTTv5,
        )

    client.username_pw_set(user_sub, token)

    if use_tls:
        ctx = ssl.create_default_context()
        if args.insecure:
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
        client.tls_set_context(ctx)

    device_id = args.device_id
    topic_cmd  = f"flower/{device_id}/down/cmd"
    topic_resp = f"flower/{device_id}/up/cmd_response"

    done      = threading.Event()
    ota_result = {"status": None}

    def on_connect(c, userdata, flags, reason_code, properties=None):
        rc = reason_code if isinstance(reason_code, int) else reason_code.value
        if rc != 0:
            print(f"[MQTT] connect failed rc={rc}", flush=True)
            done.set()
            return
        print(f"[MQTT] connected", flush=True)
        c.subscribe(topic_resp, qos=1)

        # Send OTA command
        cmd = Command()
        cmd.ota.url = fw_url
        cmd.role = 1  # CommandRole.APP = 1
        payload = cmd.SerializeToString()
        c.publish(topic_cmd, payload, qos=1)
        print(f"[OTA]  sent → {device_id}  url={fw_url}", flush=True)

        if args.wait == 0:
            done.set()

    def on_message(c, userdata, msg):
        cr = CommandResponse()
        try:
            cr.ParseFromString(msg.payload)
        except Exception as e:
            print(f"[RESP] parse error: {e}", flush=True)
            return

        which = cr.WhichOneof("payload")
        if which != "ota":
            return

        status_map = {0: "开始下载", 1: f"下载中 {cr.ota.progress}%", 2: "OTA 成功，设备重启", 3: f"OTA 失败: {cr.ota.message}"}
        status_str = status_map.get(cr.ota.status, f"未知状态 {cr.ota.status}")
        print(f"[RESP] {status_str}", flush=True)

        if cr.ota.status in (2, 3):
            ota_result["status"] = cr.ota.status
            done.set()

    def on_disconnect(c, userdata, flags, reason_code=None, properties=None):
        rc = reason_code if isinstance(reason_code, int) else (reason_code.value if reason_code else -1)
        if not done.is_set():
            print(f"[MQTT] disconnected rc={rc}", flush=True)

    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect

    connect_props = Properties(PacketTypes.CONNECT)
    connect_props.UserProperty = [("project", "flower")]

    print(f"[MQTT] connecting to {host}:{broker_port} …", flush=True)
    client.connect(host, broker_port, keepalive=60, properties=connect_props)
    client.loop_start()

    wait_s = args.wait if args.wait > 0 else 10
    done.wait(timeout=wait_s)
    client.loop_stop()
    client.disconnect()

    final = ota_result["status"]
    if final == 2:
        print("[OTA]  完成 ✓", flush=True)
        sys.exit(0)
    elif final == 3:
        print("[OTA]  失败 ✗", flush=True)
        sys.exit(1)
    elif args.wait == 0:
        print("[OTA]  指令已发送（未等待响应）", flush=True)
        sys.exit(0)
    else:
        print("[OTA]  超时：未收到设备响应", flush=True)
        sys.exit(2)


if __name__ == "__main__":
    main()
