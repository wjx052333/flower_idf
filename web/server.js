#!/usr/bin/env node
/**
 * server.js — Flower OTA local file server
 *
 * Usage:  node server.js [port]   (default: 8000)
 *
 * Endpoints:
 *   GET  /api/firmware              → list firmware files (build dir + uploads)
 *   GET  /firmware/<name>.bin       → serve firmware (build dir first, then uploads)
 *   POST /upload?name=<name>.bin    → save uploaded binary; returns { url, size }
 *   GET  /*                         → static files (index.html, *.js, *.json …)
 */

const http = require('http');
const fs   = require('fs');
const path = require('path');
const os   = require('os');

const PORT         = parseInt(process.argv[2]) || 8000;
const FIRMWARE_DIR = path.join(__dirname, 'firmware');          // uploaded files
const BUILD_DIR    = path.join(__dirname, '../relay/build');    // idf.py build output

// Non-app .bin files that live in the build root — skip these for OTA
const SKIP_BINS = new Set(['ota_data_initial.bin', 'partition-table.bin', 'bootloader.bin']);

if (!fs.existsSync(FIRMWARE_DIR)) fs.mkdirSync(FIRMWARE_DIR);

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js':   'application/javascript',
  '.json': 'application/json',
  '.css':  'text/css',
  '.bin':  'application/octet-stream',
};

function localIP() {
  for (const iface of Object.values(os.networkInterfaces())) {
    for (const info of iface) {
      if (info.family === 'IPv4' && !info.internal) return info.address;
    }
  }
  return '127.0.0.1';
}

/** Scan build dir for app-level .bin files (e.g. flower.bin). */
function scanBuildFirmware() {
  if (!fs.existsSync(BUILD_DIR)) return [];
  return fs.readdirSync(BUILD_DIR)
    .filter(f => {
      if (!f.endsWith('.bin')) return false;
      if (SKIP_BINS.has(f))   return false;
      return fs.statSync(path.join(BUILD_DIR, f)).isFile();
    })
    .map(f => {
      const s = fs.statSync(path.join(BUILD_DIR, f));
      return { name: f, size: s.size, mtime: s.mtimeMs, source: 'build' };
    });
}

/** Scan upload dir for manually-uploaded .bin files. */
function scanUploadFirmware() {
  if (!fs.existsSync(FIRMWARE_DIR)) return [];
  return fs.readdirSync(FIRMWARE_DIR)
    .filter(f => f.endsWith('.bin') && fs.statSync(path.join(FIRMWARE_DIR, f)).isFile())
    .map(f => {
      const s = fs.statSync(path.join(FIRMWARE_DIR, f));
      return { name: f, size: s.size, mtime: s.mtimeMs, source: 'upload' };
    });
}

/** Resolve a firmware filename to an absolute path (build dir takes priority). */
function resolveFirmware(name) {
  const inBuild  = path.join(BUILD_DIR,    name);
  const inUpload = path.join(FIRMWARE_DIR, name);
  if (fs.existsSync(inBuild))  return inBuild;
  if (fs.existsSync(inUpload)) return inUpload;
  return null;
}

const server = http.createServer((req, res) => {
  res.setHeader('Access-Control-Allow-Origin',  '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  if (req.method === 'OPTIONS') { res.writeHead(204); res.end(); return; }

  const url = new URL(req.url, 'http://localhost');
  const pth = url.pathname;

  /* ── GET /api/firmware ──────────────────────────────────────────────── */
  if (req.method === 'GET' && pth === '/api/firmware') {
    // Merge: build files first, then uploads (skip duplicates by name)
    const build   = scanBuildFirmware();
    const uploads = scanUploadFirmware().filter(u => !build.find(b => b.name === u.name));
    const list    = [...build, ...uploads].sort((a, b) => b.mtime - a.mtime);
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(list));
    return;
  }

  /* ── POST /upload?name=xxx.bin ──────────────────────────────────────── */
  if (req.method === 'POST' && pth === '/upload') {
    const name = path.basename(url.searchParams.get('name') || 'firmware.bin');
    const dest = path.join(FIRMWARE_DIR, name);
    const chunks = [];
    req.on('data', d => chunks.push(d));
    req.on('end', () => {
      const buf = Buffer.concat(chunks);
      fs.writeFile(dest, buf, err => {
        if (err) {
          res.writeHead(500, { 'Content-Type': 'application/json' });
          res.end(JSON.stringify({ error: err.message }));
          return;
        }
        const downloadUrl = `http://${localIP()}:${PORT}/firmware/${name}`;
        console.log(`[upload] ${name}  ${(buf.length/1024).toFixed(1)} KB  →  ${downloadUrl}`);
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ url: downloadUrl, name, size: buf.length }));
      });
    });
    req.on('error', e => { res.writeHead(500); res.end(e.message); });
    return;
  }

  /* ── GET /firmware/<name> ───────────────────────────────────────────── */
  if (req.method === 'GET' && pth.startsWith('/firmware/')) {
    const name = path.basename(pth);
    const file = resolveFirmware(name);
    if (!file) { res.writeHead(404); res.end('not found'); return; }
    const stat = fs.statSync(file);
    res.writeHead(200, {
      'Content-Type':   'application/octet-stream',
      'Content-Length': stat.size,
    });
    fs.createReadStream(file).pipe(res);
    console.log(`[serve]  ${name}  (${(stat.size/1024).toFixed(1)} KB)`);
    return;
  }

  /* ── GET static files ───────────────────────────────────────────────── */
  if (req.method === 'GET') {
    const rel  = pth === '/' ? 'index.html' : pth.slice(1);
    const file = path.join(__dirname, rel);
    if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
      res.writeHead(404); res.end('not found'); return;
    }
    const ext = path.extname(file);
    res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream' });
    fs.createReadStream(file).pipe(res);
    return;
  }

  res.writeHead(405); res.end('method not allowed');
});

server.listen(PORT, '0.0.0.0', () => {
  const ip = localIP();
  console.log('\nFlower OTA Server');
  console.log(`  Local:    http://localhost:${PORT}`);
  console.log(`  Network:  http://${ip}:${PORT}`);
  console.log(`  Build:    ${BUILD_DIR}`);
  console.log('Ctrl+C to stop\n');
});
