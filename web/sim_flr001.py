#!/usr/bin/env python3
"""
sim_flr001.py — 模拟 flr001 设备上线，每 30 秒发送 StatusReport，接收并响应 relay_control 命令。

Usage:
  python3 sim_flr001.py [--broker mqtt://HOST:1883] [--insecure]

环境变量（可选，优先级高于默认值）：
  DEVICE_ID      设备 ID（默认 flr001）
  DEVICE_SECRET  设备密钥（默认 init.sql 中的值）
  MQTT_BROKER    Broker URL（默认 mqtt://10.6.64.55:1883）

生成 flower_pb2.py（需要 protoc）：
  protoc --python_out=. flower.proto
  （flower.proto 位于 protocol/ 目录）
"""

import argparse
import hashlib
import hmac as _hmac
import os
import ssl
import sys
import threading
import time

import paho.mqtt.client as mqtt
from paho.mqtt.properties import Properties
from paho.mqtt.packettypes import PacketTypes

from flower_pb2 import (
    StatusReport,
    Command,
    CommandResponse,
    CommandRole,
)

DEVICE_ID     = os.environ.get("DEVICE_ID",     "flr001")
DEVICE_SECRET = os.environ.get("DEVICE_SECRET",
    "37e73a61317f6ca96f054ff7025c18ac67d0ccf636f376c78bb800fd9e8d2eb3")

TOPIC_CMD    = f"flower/{DEVICE_ID}/down/cmd"
TOPIC_STATUS = f"flower/{DEVICE_ID}/up/status"
TOPIC_RESP   = f"flower/{DEVICE_ID}/up/cmd_response"

STATUS_INTERVAL = 30  # seconds


def calc_signature(device_id: str, secret: str, ts_ms: int) -> str:
    plaintext = f"clientId{device_id}timestamp{ts_ms}"
    return _hmac.new(secret.encode(), plaintext.encode(), hashlib.sha256).hexdigest()


def publish_status_report(client: mqtt.Client) -> None:
    sr = StatusReport()
    sr.timestamp = int(time.time() * 1000)
    client.publish(TOPIC_STATUS, sr.SerializeToString(), qos=1)
    print(f"[SR] timestamp={sr.timestamp}", flush=True)


def on_connect(client, userdata, flags, reason_code, properties=None):
    # paho 2.x passes a ReasonCode; paho 1.x passes an int
    rc = reason_code if isinstance(reason_code, int) else reason_code.value
    if rc == 0:
        print(f"[MQTT] connected as {DEVICE_ID}", flush=True)
        client.subscribe(TOPIC_CMD, qos=1)
        publish_status_report(client)
    else:
        print(f"[MQTT] connect failed rc={rc}", flush=True)


def on_message(client, userdata, msg):
    cmd = Command()
    try:
        cmd.ParseFromString(msg.payload)
    except Exception as e:
        print(f"[CMD] parse error: {e}", flush=True)
        return

    which = cmd.WhichOneof("payload")
    print(f"[CMD] received '{which}'", flush=True)

    resp = CommandResponse()

    if which == "relay_control":
        rc_msg = cmd.relay_control
        print(f"[CMD] relay_control on={rc_msg.on} duration_ms={rc_msg.duration_ms}", flush=True)
        # CMD_RESULT_OK = 0
        resp.relay_control.result = 0
        resp.relay_control.reason = "ok"
        client.publish(TOPIC_RESP, resp.SerializeToString(), qos=1)
        print(f"[CMD] relay response sent", flush=True)
    else:
        print(f"[CMD] unsupported '{which}', no response", flush=True)


def on_disconnect(client, userdata, flags, reason_code=None, properties=None):
    rc = reason_code if isinstance(reason_code, int) else (reason_code.value if reason_code else -1)
    print(f"[MQTT] disconnected rc={rc}", flush=True)


def make_client() -> tuple[mqtt.Client, str, int]:
    ts_ms    = int(time.time() * 1000)
    username = f"{DEVICE_ID}|{ts_ms}"
    password = calc_signature(DEVICE_ID, DEVICE_SECRET, ts_ms)

    try:
        client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=DEVICE_ID,
            protocol=mqtt.MQTTv5,
        )
    except AttributeError:
        client = mqtt.Client(client_id=DEVICE_ID, protocol=mqtt.MQTTv5)

    client.username_pw_set(username, password)
    client.on_connect    = on_connect
    client.on_message    = on_message
    client.on_disconnect = on_disconnect
    return client


def main() -> None:
    parser = argparse.ArgumentParser(description="Flower flr001 device simulator")
    parser.add_argument("--broker",   default=os.environ.get("MQTT_BROKER", "mqtt://10.6.64.55:1883"))
    parser.add_argument("--insecure", action="store_true", help="Skip TLS cert verification")
    args = parser.parse_args()

    broker = args.broker
    use_tls = broker.startswith("mqtts://") or broker.startswith("ssl://")
    host_port = broker.removeprefix("mqtts://").removeprefix("ssl://").removeprefix("mqtt://")
    host, port_str = host_port.rsplit(":", 1) if ":" in host_port else (host_port, "1883" if not use_tls else "8883")
    port = int(port_str)

    client = make_client()

    if use_tls:
        ctx = ssl.create_default_context()
        if args.insecure:
            ctx.check_hostname = False
            ctx.verify_mode    = ssl.CERT_NONE
        client.tls_set_context(ctx)

    # Pass project="flower" as MQTT5 User Property so the backend can correctly
    # set the topic prefix for APP client authorization.
    connect_props = Properties(PacketTypes.CONNECT)
    connect_props.UserProperty = [("project", "flower")]

    print(f"[SIM] connecting to {host}:{port} as {DEVICE_ID}", flush=True)
    client.connect(host, port, keepalive=60, properties=connect_props)

    # Periodic status report thread
    def _sr_loop():
        while True:
            time.sleep(STATUS_INTERVAL)
            if client.is_connected():
                publish_status_report(client)

    threading.Thread(target=_sr_loop, daemon=True).start()

    print(f"[SIM] running — Ctrl+C to stop", flush=True)
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[SIM] stopped", flush=True)
        client.disconnect()


if __name__ == "__main__":
    main()
