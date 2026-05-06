#!/usr/bin/env python3
"""
server.py — Flower 设备 Web 服务器

提供静态文件 + OTA 固件服务，浏览器直连 backend 和 EMQX WSS。

Usage:
  python3 server.py [--port 9100] [--no-tls]

OTA 接口:
  GET  /api/firmware              → 扫描 ../relay/build 和 ./firmware/ 返回固件列表
  GET  /firmware/<name>.bin       → 下载固件（build 优先，其次 upload 目录）
  POST /upload?name=<name>.bin    → 上传固件到 ./firmware/，返回 {"url":..., "size":...}
"""

import argparse
import http.server
import json
import os
import ssl
import subprocess
import urllib.parse

STATIC_DIR   = os.path.dirname(os.path.abspath(__file__))
FIRMWARE_DIR = os.path.join(STATIC_DIR, "firmware")
BUILD_DIR    = os.path.join(STATIC_DIR, "..", "relay", "build")

# build 目录下不属于 OTA app 的 bin 文件
SKIP_BINS = {"ota_data_initial.bin", "partition-table.bin", "bootloader.bin"}

os.makedirs(FIRMWARE_DIR, exist_ok=True)

MIME = {
    ".html": "text/html; charset=utf-8",
    ".js":   "application/javascript",
    ".json": "application/json",
    ".css":  "text/css",
    ".bin":  "application/octet-stream",
}


def _scan_firmware(host: str) -> list:
    """返回固件列表，每项含 name/size/mtime/source/url。"""
    files = []

    # 1. build 目录：只取 app 级 bin
    if os.path.isdir(BUILD_DIR):
        for name in os.listdir(BUILD_DIR):
            if not name.endswith(".bin") or name in SKIP_BINS:
                continue
            fp = os.path.join(BUILD_DIR, name)
            if os.path.isfile(fp):
                st = os.stat(fp)
                files.append({
                    "name":   name,
                    "size":   st.st_size,
                    "mtime":  int(st.st_mtime * 1000),
                    "source": "build",
                    "url":    f"http://{host}/firmware/{urllib.parse.quote(name)}",
                })

    # 2. upload 目录：补充未被 build 覆盖的文件
    existing = {f["name"] for f in files}
    if os.path.isdir(FIRMWARE_DIR):
        for name in os.listdir(FIRMWARE_DIR):
            if not name.endswith(".bin") or name in existing:
                continue
            fp = os.path.join(FIRMWARE_DIR, name)
            if os.path.isfile(fp):
                st = os.stat(fp)
                files.append({
                    "name":   name,
                    "size":   st.st_size,
                    "mtime":  int(st.st_mtime * 1000),
                    "source": "upload",
                    "url":    f"http://{host}/firmware/{urllib.parse.quote(name)}",
                })

    files.sort(key=lambda f: f["mtime"], reverse=True)
    return files


def _resolve_firmware(name: str) -> str | None:
    """返回固件绝对路径，build 优先；找不到返回 None。"""
    for d in (BUILD_DIR, FIRMWARE_DIR):
        fp = os.path.join(d, name)
        if os.path.isfile(fp):
            return fp
    return None


class Handler(http.server.BaseHTTPRequestHandler):

    def _cors(self):
        self.send_header("Access-Control-Allow-Origin",  "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.end_headers()

    # ── GET ───────────────────────────────────────────────────────────────
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path   = parsed.path

        # /api/firmware — 固件列表
        if path == "/api/firmware":
            host  = self.headers.get("Host", "localhost")
            data  = json.dumps(_scan_firmware(host)).encode()
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type",   "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        # /firmware/<name> — 固件下载
        if path.startswith("/firmware/"):
            name = urllib.parse.unquote(os.path.basename(path))
            fp   = _resolve_firmware(name)
            if not fp:
                self.send_error(404, "firmware not found")
                return
            size = os.path.getsize(fp)
            self.send_response(200)
            self._cors()
            self.send_header("Content-Type",   "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.end_headers()
            with open(fp, "rb") as f:
                self.wfile.write(f.read())
            print(f"[OTA]  serve {name}  ({size//1024} KB)", flush=True)
            return

        # 静态文件
        if path in ("/", ""):
            path = "/index.html"
        filepath = os.path.join(STATIC_DIR, path.lstrip("/"))
        if not os.path.isfile(filepath):
            self.send_error(404)
            return
        _, ext = os.path.splitext(filepath)
        with open(filepath, "rb") as f:
            data = f.read()
        self.send_response(200)
        self._cors()
        self.send_header("Content-Type",   MIME.get(ext, "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    # ── POST /upload?name=xxx.bin ─────────────────────────────────────────
    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path != "/upload":
            self.send_error(404)
            return

        params  = urllib.parse.parse_qs(parsed.query)
        name    = os.path.basename((params.get("name") or ["firmware.bin"])[0])
        length  = int(self.headers.get("Content-Length", 0))
        payload = self.rfile.read(length)

        dest = os.path.join(FIRMWARE_DIR, name)
        with open(dest, "wb") as f:
            f.write(payload)

        host = self.headers.get("Host", "localhost")
        url  = f"http://{host}/firmware/{urllib.parse.quote(name)}"
        print(f"[OTA]  upload {name}  ({len(payload)//1024} KB)  → {url}", flush=True)

        resp = json.dumps({"url": url, "name": name, "size": len(payload)}).encode()
        self.send_response(200)
        self._cors()
        self.send_header("Content-Type",   "application/json")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, fmt, *args):
        print(f"[HTTP] {fmt % args}", flush=True)


def _gen_cert(cert_path: str, key_path: str) -> None:
    os.makedirs(os.path.dirname(cert_path), exist_ok=True)
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048",
         "-keyout", key_path, "-out", cert_path,
         "-days", "3650", "-nodes", "-subj", "/CN=localhost"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    print(f"[TLS]  generated self-signed cert: {cert_path}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port",   type=int, default=9100)
    parser.add_argument("--no-tls", action="store_true")
    parser.add_argument("--cert",   default=None)
    parser.add_argument("--key",    default=None)
    args = parser.parse_args()

    server = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), Handler)

    if not args.no_tls:
        cert_path = args.cert or os.path.join(STATIC_DIR, "local_cert/web.pem")
        key_path  = args.key  or os.path.join(STATIC_DIR, "local_cert/web-key.pem")
        if not (os.path.isfile(cert_path) and os.path.isfile(key_path)):
            _gen_cert(cert_path, key_path)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(cert_path, key_path)
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
        scheme = "https"
    else:
        scheme = "http"

    print(f"[HTTP] {scheme}://0.0.0.0:{args.port}", flush=True)
    print(f"[OTA]  build dir : {os.path.abspath(BUILD_DIR)}", flush=True)
    print(f"[OTA]  upload dir: {FIRMWARE_DIR}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[HTTP] stopped", flush=True)


if __name__ == "__main__":
    main()
