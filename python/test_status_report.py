"""
MQTT StatusReport test publisher for backend verification.

Publishes flower.StatusReport with device_type="humidityX2_lux" and
3 metrics (humiL, humiD, lux) as sine waves (period=2h, different amplitudes).

Usage:
    python test_status_report.py [--host HOST]
"""

import argparse
import time
import math
import hmac
import hashlib
import ssl

import paho.mqtt.client as mqtt
import paho.mqtt.enums as enums
import paho.mqtt.properties as props

# Generated protobuf — see gen_pb.py
import flower_pb2

# ── Config ──────────────────────────────────────────────────────────────
BROKER_PORT = 8883
DEVICE_ID   = "flr002"
DEVICE_NAME = "Flower 002"
DEVICE_SECRET = "71e36e826a978057170be4c71b6e2f14dd3a65df9b10d91fa5ffec38587aedaa"

TOPIC_STATUS = f"flower/{DEVICE_ID}/up/status"

# Sine wave: period=7200s (2h), publish interval=30s
SINE_PERIOD_S = 7200
PUBLISH_INTERVAL_S = 30

# Amplitude / offset per metric
METRICS_CFG = {
    "humiL": {"amplitude": 30.0, "offset": 50.0},
    "humiD": {"amplitude": 20.0, "offset": 40.0},
    "lux":   {"amplitude": 400.0, "offset": 500.0},
}


def calc_signature(ts_ms, device_id, secret):
    plaintext = f"clientId{device_id}timestamp{ts_ms}"
    return hmac.new(secret.encode(), plaintext.encode(), hashlib.sha256).hexdigest()


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        print(f"[connected] rc={reason_code}")
    else:
        print(f"[connect failed] rc={reason_code}")


def on_disconnect(client, userdata, flags, reason_code, properties):
    print(f"[disconnected] rc={reason_code}")


def build_status_report():
    """Build a StatusReport protobuf with current sine-wave metrics."""
    now_s = time.time()
    phase = (now_s % SINE_PERIOD_S) / SINE_PERIOD_S * 2.0 * math.pi

    sr = flower_pb2.StatusReport()
    sr.timestamp = int(now_s * 1000)
    sr.device_type = "humidityX2_lux"
    sr.version.major = 1
    sr.version.minor = 0
    sr.version.patch = 2

    for key, cfg in METRICS_CFG.items():
        val = cfg["offset"] + cfg["amplitude"] * math.sin(phase)
        m = sr.metrics.add()
        m.key = key
        m.double_value = round(val, 3)

    return sr


def main():
    parser = argparse.ArgumentParser(description="MQTT StatusReport test publisher")
    parser.add_argument("--host", default="58.87.80.78", help="MQTT broker host (default: 58.87.80.78)")
    args = parser.parse_args()
    broker_host = args.host

    ts_ms = int(time.time() * 1000)
    username = f"{DEVICE_ID}|{ts_ms}"
    password = calc_signature(ts_ms, DEVICE_ID, DEVICE_SECRET)

    client = mqtt.Client(
        callback_api_version=enums.CallbackAPIVersion.VERSION2,
        client_id=DEVICE_ID,
        protocol=enums.MQTTProtocolVersion.MQTTv5,
    )

    # TLS: skip cert verification (match ESP32 skip_cert_common_name_check)
    tls_context = ssl.create_default_context()
    tls_context.check_hostname = False
    tls_context.verify_mode = ssl.CERT_NONE
    client.tls_set_context(tls_context)

    client.username_pw_set(username, password)

    connect_props = props.Properties(props.PacketTypes.CONNECT)
    connect_props.UserProperty = ("project", "flower")
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect

    print(f"Connecting to {broker_host}:{BROKER_PORT} ...")
    client.connect(broker_host, BROKER_PORT, keepalive=60, clean_start=True,
                   properties=connect_props)
    client.loop_start()

    time.sleep(1)

    print("Publishing StatusReport every 30s (sine period=2h). Ctrl+C to stop.")
    try:
        while True:
            sr = build_status_report()
            data = sr.SerializeToString()

            publish_props = props.Properties(props.PacketTypes.PUBLISH)
            client.publish(TOPIC_STATUS, data, qos=1, properties=publish_props)

            vals = {m.key: m.double_value for m in sr.metrics}
            print(f"[published] ts={sr.timestamp} {vals}")
            time.sleep(PUBLISH_INTERVAL_S)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()