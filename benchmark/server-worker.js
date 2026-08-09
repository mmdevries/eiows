'use strict';

const http = require('node:http');

const [implementation, modulePath, compressionArgument] = process.argv.slice(2);
const compression = compressionArgument === 'on';

if (!implementation || !modulePath || typeof process.send !== 'function') {
    throw new Error('server-worker must be started by benchmark/run.js');
}

let sockets = new Set();
let messages = 0;
let bytes = 0;
let peakRss = process.memoryUsage().rss;
let measurementStart = null;

function collectGarbage() {
    if (typeof global.gc !== 'function') return;
    global.gc();
    global.gc();
}

function memorySnapshot() {
    const usage = process.memoryUsage();
    peakRss = Math.max(peakRss, usage.rss);
    return {
        rss: usage.rss,
        heapUsed: usage.heapUsed,
        external: usage.external,
        arrayBuffers: usage.arrayBuffers || 0
    };
}

function sendResponse(id, type, value) {
    process.send({ id, type, value });
}

async function main() {
    const websocketModule = require(modulePath);
    const Server = websocketModule.WebSocketServer || websocketModule.Server;
    if (typeof Server !== 'function') {
        throw new TypeError(`${implementation} does not export a WebSocket server`);
    }

    const websocketServer = new Server({
        noServer: true,
        maxPayload: 16 * 1024 * 1024,
        perMessageDeflate: compression ? {
            threshold: 0,
            clientNoContextTakeover: true,
            serverNoContextTakeover: true
        } : false
    });
    const httpServer = http.createServer();

    httpServer.on('upgrade', (request, socket, head) => {
        websocketServer.handleUpgrade(request, socket, head, (webSocket) => {
            sockets.add(webSocket);
            webSocket.on('error', () => {});
            webSocket.on('close', () => sockets.delete(webSocket));
            webSocket.on('message', (data, isBinary) => {
                messages++;
                bytes += data.byteLength === undefined
                    ? Buffer.byteLength(data)
                    : data.byteLength;
                try {
                    const payload = isBinary || typeof data === 'string'
                        ? data
                        : data.toString();
                    if (implementation === 'ws') {
                        webSocket.send(payload, {
                            binary: isBinary,
                            compress: compression
                        });
                    } else {
                        webSocket.send(payload, { compress: compression });
                    }
                } catch {
                    try {
                        webSocket.terminate();
                    } catch {}
                }
            });
        });
    });

    const peakTimer = setInterval(() => {
        peakRss = Math.max(peakRss, process.memoryUsage().rss);
    }, 25);
    peakTimer.unref();

    process.on('message', async (message) => {
        const { id, command } = message;
        if (command === 'snapshot') {
            if (message.gc) collectGarbage();
            sendResponse(id, 'snapshot', {
                memory: memorySnapshot(),
                connections: sockets.size,
                messages,
                bytes,
                peakRss
            });
            return;
        }
        if (command === 'begin-measurement') {
            collectGarbage();
            const usage = process.memoryUsage();
            peakRss = usage.rss;
            messages = 0;
            bytes = 0;
            measurementStart = {
                cpu: process.cpuUsage(),
                time: process.hrtime.bigint(),
                memory: {
                    rss: usage.rss,
                    heapUsed: usage.heapUsed,
                    external: usage.external,
                    arrayBuffers: usage.arrayBuffers || 0
                }
            };
            sendResponse(id, 'begin-measurement', measurementStart.memory);
            return;
        }
        if (command === 'end-measurement') {
            const elapsedNs = measurementStart
                ? process.hrtime.bigint() - measurementStart.time
                : 0n;
            const cpu = measurementStart
                ? process.cpuUsage(measurementStart.cpu)
                : { user: 0, system: 0 };
            sendResponse(id, 'end-measurement', {
                memory: memorySnapshot(),
                startMemory: measurementStart && measurementStart.memory,
                peakRss,
                messages,
                bytes,
                elapsedMs: Number(elapsedNs) / 1e6,
                cpuUserMicros: cpu.user,
                cpuSystemMicros: cpu.system
            });
            return;
        }
        if (command === 'shutdown') {
            clearInterval(peakTimer);
            for (const webSocket of sockets) {
                try {
                    webSocket.terminate();
                } catch {
                    webSocket.close();
                }
            }
            sockets = new Set();
            try {
                websocketServer.close();
            } catch {}
            httpServer.close(() => {
                sendResponse(id, 'shutdown', true);
                process.disconnect();
            });
        }
    });

    httpServer.listen(0, '127.0.0.1', () => {
        collectGarbage();
        const nativeSupport = implementation === 'ws' ? {
            bufferutilLoaded: Object.keys(require.cache).some((filename) =>
                filename.includes('/bufferutil/') && filename.endsWith('.node')),
            utf8Path: typeof require('node:buffer').isUtf8 === 'function'
                ? 'node:buffer.isUtf8'
                : 'utf-8-validate'
        } : null;
        process.send({
            type: 'ready',
            value: {
                port: httpServer.address().port,
                pid: process.pid,
                memory: memorySnapshot(),
                nativeSupport
            }
        });
    });
}

main().catch((error) => {
    process.send({ type: 'fatal', value: error.stack || String(error) });
    process.exitCode = 1;
});
