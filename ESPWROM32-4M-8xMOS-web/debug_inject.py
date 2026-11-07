#!/usr/bin/env python3
"""
debug_inject.py — 给本地模拟器（mock_device.py）发送调试注入

Usage:
  python debug_inject.py soil --pct 35          # 按湿度百分比注入（按当前标定换算成 AD）
  python debug_inject.py soil --raw 2500        # 直接注入 ADC 原始值 0-4095
  python debug_inject.py reed open              # 开门（干簧管断开）
  python debug_inject.py reed close             # 关门（干簧管闭合）

  --url http://127.0.0.1:8080   模拟器地址（默认）
"""

import argparse
import json
import sys
import urllib.error
import urllib.request


def call(url, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url + path, data,
                                 {"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        sys.exit(f"HTTP {e.code}: {e.read().decode('utf-8', 'replace')}")
    except urllib.error.URLError as e:
        sys.exit(f"连不上模拟器 {url}：{e.reason}（先运行 python mock_device.py）")


def cmd_soil(args):
    if args.raw is not None:
        raw = args.raw
    else:
        st = call(args.url, "/debug/api/state")
        dry, wet = st["raw_dry"], st["raw_wet"]
        raw = round(dry - args.pct * (dry - wet) / 100)
    st = call(args.url, "/debug/api/inject", {"raw_low": raw})
    print(f"土壤湿度已注入：AD={st['sensors']['raw_low']} → {st['humidity']}%")


def cmd_reed(args):
    closed = args.state == "close"
    call(args.url, "/debug/api/inject", {"door_closed": closed})
    d = call(args.url, "/api/sensors")
    print(f"干簧管：{'闭合（门关）' if d['door_closed'] else '断开（门开）'}，"
          f"触发 {d['reed_open_cnt']} 次 / 脉冲 {d['reed_isr_cnt']} 次")


def pct(s):
    v = float(s)
    if not 0 <= v <= 100:
        raise argparse.ArgumentTypeError("0-100")
    return v


def raw(s):
    v = int(s)
    if not 0 <= v <= 4095:
        raise argparse.ArgumentTypeError("0-4095")
    return v


def main():
    ap = argparse.ArgumentParser(description="向本地模拟器注入调试数据")
    ap.add_argument("--url", default="http://127.0.0.1:8080", help="模拟器地址")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("soil", help="注入土壤湿度")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--pct", type=pct, help="湿度百分比 0-100")
    g.add_argument("--raw", type=raw, help="ADC 原始值 0-4095")
    p.set_defaults(func=cmd_soil)

    p = sub.add_parser("reed", help="开门/关门")
    p.add_argument("state", choices=["open", "close"])
    p.set_defaults(func=cmd_reed)

    args = ap.parse_args()
    args.url = args.url.rstrip("/")
    args.func(args)


if __name__ == "__main__":
    main()
