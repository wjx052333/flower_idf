/**
 * test_device_discovery.js
 * 设备发现报文格式校验与 UDP 监听/探测上位机离线测试工具
 */
const dgram = require('dgram');

// 1. 协议单元测试函数
function parseDiscoveryPacket(msg) {
    if (typeof msg !== 'string') return null;
    const parts = msg.trim().split(':');
    if (parts.length !== 4) return null;
    const [magic, hostname, ip, portStr] = parts;
    if (magic !== 'FLOWER') return null;
    const port = parseInt(portStr, 10);
    if (isNaN(port) || port <= 0 || port > 65535) return null;
    // 简易 IPv4 格式检查
    const ipRegex = /^(\d{1,3}\.){3}\d{1,3}$/;
    if (!ipRegex.test(ip)) return null;

    return { magic, hostname, ip, port };
}

// 2. 运行协议格式自测
function runUnitTests() {
    console.log("Running discovery protocol unit tests...");

    // 正确用例
    const validPacket = "FLOWER:rel_flr006:192.168.1.120:80";
    const res1 = parseDiscoveryPacket(validPacket);
    if (!res1 || res1.hostname !== 'rel_flr006' || res1.ip !== '192.168.1.120' || res1.port !== 80) {
        throw new Error("Failed valid packet test");
    }

    // 错误用例：非法 Header
    if (parseDiscoveryPacket("OTHER:rel_flr006:192.168.1.120:80") !== null) {
        throw new Error("Failed invalid header test");
    }

    // 错误用例：缺少字段
    if (parseDiscoveryPacket("FLOWER:rel_flr006:192.168.1.120") !== null) {
        throw new Error("Failed missing field test");
    }

    // 错误用例：非法端口
    if (parseDiscoveryPacket("FLOWER:rel_flr006:192.168.1.120:99999") !== null) {
        throw new Error("Failed invalid port test");
    }

    console.log("PASS: Discovery protocol unit tests passed successfully!");
}

runUnitTests();

// 3. 命令行参数驱动：若传入 --listen，则作为真实 UDP 监听器运行 3 秒测试
if (process.argv.includes('--listen')) {
    const PORT = 3333;
    const socket = dgram.createSocket({ type: 'udp4', reuseAddr: true });

    socket.on('message', (msg, rinfo) => {
        const text = msg.toString();
        console.log(`[UDP Recv] From ${rinfo.address}:${rinfo.port} -> ${text}`);
        const parsed = parseDiscoveryPacket(text);
        if (parsed) {
            console.log(`[Device Found] Hostname: ${parsed.hostname}, IP: ${parsed.ip}, Port: ${parsed.port}`);
        }
    });

    socket.bind(PORT, () => {
        socket.setBroadcast(true);
        console.log(`[Listener] Listening on 0.0.0.0:${PORT}... Sending FLOWER_DISCOVER probe...`);
        const probe = Buffer.from('FLOWER_DISCOVER');
        socket.send(probe, 0, probe.length, PORT, '255.255.255.255');
    });

    setTimeout(() => {
        socket.close();
        console.log("[Listener] Test run finished.");
        process.exit(0);
    }, 2000);
}
