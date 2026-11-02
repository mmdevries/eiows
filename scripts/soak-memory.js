'use strict';

// Keep the load generator in a child process so samples describe server memory.
const assert = require('node:assert/strict');
const { fork } = require('node:child_process');
const { once } = require('node:events');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const net = require('node:net');
const path = require('node:path');
const tls = require('node:tls');
const { setImmediate: nextTurn, setTimeout: delay } = require('node:timers/promises');
const zlib = require('node:zlib');

function integer(name, fallback, maximum) {
    const value = process.env[name] === undefined ? fallback : Number(process.env[name]);
    if (!Number.isSafeInteger(value) || value < 1 || value > maximum) {
        throw new RangeError(`${name} must be an integer between 1 and ${maximum}`);
    }
    return value;
}

function frame(payload, opcode, compressed = false) {
    const headerBytes = payload.length < 126 ? 2 : payload.length <= 65535 ? 4 : 10;
    const result = Buffer.alloc(headerBytes + 4 + payload.length);
    result[0] = 0x80 | opcode | (compressed ? 0x40 : 0);
    if (headerBytes === 2) result[1] = 0x80 | payload.length;
    else if (headerBytes === 4) {
        result[1] = 0xfe;
        result.writeUInt16BE(payload.length, 2);
    } else {
        result[1] = 0xff;
        result.writeBigUInt64BE(BigInt(payload.length), 2);
    }
    // A zero mask is valid and keeps repeated runs deterministic.
    payload.copy(result, headerBytes + 4);
    return result;
}

async function clientConnection(config, payload, packet) {
    const socket = config.secure
        ? tls.connect({ host: '127.0.0.1', port: config.port, rejectUnauthorized: false })
        : net.connect({ host: '127.0.0.1', port: config.port });
    let incoming = Buffer.alloc(0);
    let upgraded = false;
    let received = 0;
    let receivedClose = false;
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => finish(new Error('soak client timed out')), 20000);
        let finished = false;
        function finish(error) {
            if (finished) return;
            finished = true;
            clearTimeout(timer);
            socket.destroy();
            if (error) reject(error);
            else resolve();
        }
        socket.on('error', finish);
        socket.on('close', () => {
            if (received !== config.messages || !receivedClose) {
                finish(new Error(`incomplete soak exchange: ${received}/${config.messages}`));
            } else finish();
        });
        socket.once(config.secure ? 'secureConnect' : 'connect', () => {
            socket.write('GET / HTTP/1.1\r\nHost: localhost\r\n' +
                'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
                'Sec-WebSocket-Version: 13\r\n' +
                'Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n' +
                (config.compression ? 'Sec-WebSocket-Extensions: permessage-deflate; ' +
                    'client_no_context_takeover; server_no_context_takeover\r\n' : '') +
                '\r\n');
        });
        socket.on('data', (data) => {
            try {
                incoming = Buffer.concat([incoming, data]);
                if (!upgraded) {
                    const end = incoming.indexOf('\r\n\r\n');
                    if (end < 0) return;
                    const headers = incoming.subarray(0, end).toString();
                    assert.match(headers, /^HTTP\/1\.1 101 /);
                    if (config.compression) assert.match(headers, /Sec-WebSocket-Extensions:/);
                    incoming = incoming.subarray(end + 4);
                    upgraded = true;
                    for (let index = 0; index < config.messages; index++) socket.write(packet);
                }
                while (incoming.length >= 2) {
                    const opcode = incoming[0] & 15;
                    let length = incoming[1] & 127;
                    let offset = 2;
                    if (length === 126) {
                        if (incoming.length < 4) return;
                        length = incoming.readUInt16BE(2);
                        offset = 4;
                    } else if (length === 127) {
                        if (incoming.length < 10) return;
                        const value = incoming.readBigUInt64BE(2);
                        assert.ok(value <= BigInt(config.payloadBytes * 2 + 1024));
                        length = Number(value);
                        offset = 10;
                    }
                    if (incoming.length < offset + length) return;
                    const body = incoming.subarray(offset, offset + length);
                    if (opcode === 2) {
                        assert.equal(incoming[0], config.compression ? 0xc2 : 0x82);
                        const decoded = config.compression ? zlib.inflateRawSync(
                            Buffer.concat([body, Buffer.from([0, 0, 255, 255])]), {
                                finishFlush: zlib.constants.Z_SYNC_FLUSH,
                                maxOutputLength: config.payloadBytes
                            }) : body;
                        assert.deepEqual(decoded, payload);
                        assert.ok(++received <= config.messages);
                        if (received === config.messages) {
                            socket.write(frame(Buffer.from([3, 232]), 8));
                        }
                    } else {
                        assert.equal(opcode, 8);
                        assert.equal(received, config.messages);
                        assert.deepEqual(body, Buffer.from([3, 232]));
                        receivedClose = true;
                        socket.end();
                    }
                    incoming = incoming.subarray(offset + length);
                }
            } catch (error) { finish(error); }
        });
    });
}

async function clientRound(config) {
    const payload = Buffer.alloc(config.payloadBytes);
    let state = 0x91e10da5;
    for (let index = 0; index < payload.length; index++) {
        state ^= state << 13;
        state ^= state >>> 17;
        state ^= state << 5;
        payload[index] = state;
    }
    const encoded = config.compression
        ? zlib.deflateRawSync(payload, { finishFlush: zlib.constants.Z_SYNC_FLUSH }).subarray(0, -4)
        : payload;
    const packet = frame(encoded, 2, config.compression);
    await Promise.all(Array.from({ length: config.connections }, () =>
        clientConnection(config, payload, packet)));
}

function runRound(config) {
    return new Promise((resolve, reject) => {
        const child = fork(__filename, ['--client', JSON.stringify(config)], {
            execArgv: [], stdio: ['ignore', 'ignore', 'pipe', 'ipc']
        });
        let stderr = '';
        const timeout = setTimeout(() => child.kill(), 30000);
        child.stderr.on('data', (chunk) => { stderr = (stderr + chunk).slice(-8192); });
        child.once('error', (error) => { clearTimeout(timeout); reject(error); });
        child.once('exit', (code, signal) => {
            clearTimeout(timeout);
            if (code !== 0) reject(new Error(`soak clients failed (${code ?? signal}): ${stderr}`));
            else resolve();
        });
    });
}

async function memorySample() {
    for (let index = 0; index < 3; index++) {
        await nextTurn();
        global.gc();
    }
    let descriptors = null;
    for (const directory of ['/proc/self/fd', '/dev/fd']) {
        try { descriptors = fs.readdirSync(directory).length; break; } catch {}
    }
    return { ...process.memoryUsage(), descriptors };
}

async function main() {
    if (!global.gc) throw new Error('run the soak test with node --expose-gc');
    const eiows = require('..');
    const config = {
        connections: integer('SOAK_CONNECTIONS', 100, 10000),
        messages: integer('SOAK_MESSAGES', 4, 1000),
        payloadBytes: integer('SOAK_PAYLOAD_BYTES', 65536, 1024 * 1024)
    };
    const rounds = integer('SOAK_ROUNDS', 25, 100000);
    const warmupRounds = integer('SOAK_WARMUP_ROUNDS', 5, 1000);
    const maxGrowth = integer('SOAK_MAX_RSS_GROWTH_MIB', 64, 65536) * 1024 * 1024;
    const fixtures = path.join(__dirname, '..', 'test', 'fixtures');
    for (const secure of [false, true]) {
        for (const compression of [false, true]) {
            const wsServer = new eiows.Server({
                perMessageDeflate: compression ? { threshold: 0 } : false
            });
            const server = secure ? https.createServer({
                key: fs.readFileSync(path.join(fixtures, 'key.pem')),
                cert: fs.readFileSync(path.join(fixtures, 'cert.pem'))
            }) : http.createServer();
            server.on('upgrade', (request, socket, head) => {
                wsServer.handleUpgrade(request, socket, head, (ws) => {
                    ws.on('error', () => {});
                    ws.on('message', (data) => ws.send(data, { compress: compression }));
                });
            });
            const label = `${secure ? 'tls' : 'tcp'}/${compression ? 'deflate' : 'plain'}`;
            try {
                server.listen(0, '127.0.0.1');
                await once(server, 'listening');
                const runConfig = { ...config, port: server.address().port, secure, compression };
                for (let warmup = 0; warmup < warmupRounds; warmup++) await runRound(runConfig);
                const baseline = await memorySample();
                process.stdout.write(JSON.stringify({ mode: label, round: 0, ...baseline }) + '\n');
                for (let round = 1; round <= rounds; round++) {
                    await runRound(runConfig);
                    const deadline = Date.now() + 3000;
                    while (wsServer._clients.size && Date.now() < deadline) await delay(10);
                    assert.equal(wsServer._clients.size, 0, 'connections must be released after each round');
                    const sample = await memorySample();
                    const rssGrowth = sample.rss - baseline.rss;
                    process.stdout.write(JSON.stringify({ mode: label, round, ...sample, rssGrowth }) + '\n');
                    assert.ok(rssGrowth <= maxGrowth, `${label}: RSS grew by ${rssGrowth} bytes`);
                    assert.ok(sample.external - baseline.external <= maxGrowth,
                        `${label}: external memory exceeded the growth limit`);
                    if (sample.descriptors !== null && baseline.descriptors !== null) {
                        assert.ok(sample.descriptors <= baseline.descriptors + 4,
                            `${label}: file descriptors accumulate across connection cycles`);
                    }
                }
            } finally {
                for (const ws of wsServer._clients) ws.terminate();
                await new Promise((resolve) => wsServer.close(resolve));
                await new Promise((resolve) => server.close(resolve));
            }
        }
    }
}

if (process.argv[2] === '--client') {
    clientRound(JSON.parse(process.argv[3])).then(() => {
        if (process.connected) process.disconnect();
    }).catch((error) => {
        console.error(error.stack || error);
        process.exitCode = 1;
        if (process.connected) process.disconnect();
    });
} else {
    main().catch((error) => {
        console.error(error.stack || error);
        process.exitCode = 1;
    });
}
