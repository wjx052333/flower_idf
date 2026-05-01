#!/usr/bin/env python3
"""
server.py — Flower 设备 Web 服务器（仅静态文件）

浏览器直接连 backend（登录/设备列表）和 EMQX WSS（心跳订阅/relay 下发），
本服务器只提供 index.html。

Usage:
  python3 server.py [--port 9100] [--no-tls]
"""

import argparse
import http.server
import os
import ssl
import subprocess
import urllib.parse

STATIC_DIR = os.path.dirname(os.path.abspath(__file__))


class Handler(http.server.BaseHTTPRequestHandler):

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path in ("/", ""):
            path = "/index.html"

        filepath = os.path.join(STATIC_DIR, path.lstrip("/"))
        if not os.path.isfile(filepath):
            self.send_error(404)
            return

        ext_map = {".html": "text/html", ".js": "application/javascript", ".css": "text/css"}
        _, ext = os.path.splitext(filepath)
        with open(filepath, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type",   ext_map.get(ext, "application/octet-stream"))
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        print(f"[HTTP] {fmt % args}", flush=True)


def _gen_cert(cert_path: str, key_path: str) -> None:
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048",
         "-keyout", key_path, "-out", cert_path,
         "-days", "3650", "-nodes", "-subj", "/CN=localhost"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    print(f"[TLS] generated self-signed cert: {cert_path}", flush=True)


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

    print(f"[HTTP] serving {scheme}://0.0.0.0:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[HTTP] stopped", flush=True)


if __name__ == "__main__":
    main()
