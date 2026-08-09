'use strict';

const http = require('node:http');
const path = require('node:path');
const zlib = require('node:zlib');

const [implementation, modulePath, sendMode, dataMode, sourceSizeArgument] =
    process.argv.slice(2);
const sourceSize = Number(sourceSizeArgument);

if (!implementation || !modulePath || !['send', 'sendFrame'].includes(sendMode) ||
    !['text', 'app-deflate'].includes(dataMode) ||
    !Number.isInteger(sourceSize) || sourceSize <= 0 ||
    typeof process.send !== 'function') {
    throw new Error('broadcast-server-worker must be started by benchmark/broadcast-run.js');
}

const wsModule = require(path.join(__dirname, 'node_modules', 'ws'));
const sourceChunks = [];
let sourceBytes = 0;
let sourceSequence = 0;
while (sourceBytes < sourceSize) {
    const chunk = Buffer.from(`${JSON.stringify({
        event: 'update',
        room: `room-${sourceSequence % 64}`,
        sequence: sourceSequence,
        value: Math.imul(sourceSequence, 2654435761) >>> 0,
        active: (sourceSequence & 1) === 0
    })}\n`);
    sourceChunks.push(chunk);
    sourceBytes += chunk.length;
    sourceSequence++;
}
const sourceBuffer = Buffer.concat(sourceChunks, sourceBytes).subarray(0, sourceSize);
const binary = dataMode === 'app-deflate';
const payload = binary
    ? zlib.deflateRawSync(sourceBuffer)
    : sourceBuffer.toString('utf8');
const payloadBytes = Buffer.byteLength(payload);
const preEncodedFrame = wsModule.Sender.frame(payload, {
    fin: true,
    mask: false,
    opcode: binary ? 2 : 1,
    readOnly: true,
    rsv1: false
});

let sockets = new Set();
let peakRss = process.memoryUsage().rss;
let activeBroadcastRun = null;

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

function percentile(sortedValues, percentileValue) {
    if (!sortedValues.length) return 0;
    const index = Math.min(
        sortedValues.length - 1,
        Math.ceil(percentileValue * sortedValues.length) - 1
    );
    return sortedValues[index];
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
        perMessageDeflate: false
    });
    const httpServer = http.createServer();

    httpServer.on('upgrade', (request, socket, head) => {
        websocketServer.handleUpgrade(request, socket, head, (webSocket) => {
            sockets.add(webSocket);
            webSocket.on('error', () => {});
            webSocket.on('close', () => sockets.delete(webSocket));
        });
    });

    const peakTimer = setInterval(() => {
        peakRss = Math.max(peakRss, process.memoryUsage().rss);
    }, 10);
    peakTimer.unref();

    function sendOne(webSocket, callback) {
        if (sendMode === 'sendFrame') {
            webSocket._sender.sendFrame(preEncodedFrame, callback);
        } else if (implementation === 'ws') {
            webSocket.send(payload, { binary, compress: false }, callback);
        } else {
            webSocket.send(payload, { compress: false }, callback);
        }
    }

    function runBroadcasts(durationSeconds, collectLatencies) {
        if (!sockets.size) throw new Error('cannot broadcast without connections');
        if (sendMode === 'sendFrame') {
            for (const webSocket of sockets) {
                if (!webSocket._sender ||
                    typeof webSocket._sender.sendFrame !== 'function') {
                    throw new Error(`${implementation} does not support sendFrame`);
                }
            }
        }
        const start = process.hrtime.bigint();
        const deadline = start + BigInt(Math.round(durationSeconds * 1e9));
        const flushLatencies = [];
        const deliveryLatencies = [];
        let broadcasts = 0;
        let round = 0;

        return new Promise((resolve, reject) => {
            const next = () => {
                round++;
                const roundStart = process.hrtime.bigint();
                let remaining = sockets.size;
                let failed = false;
                let writesComplete = false;
                let deliveryComplete = false;
                let flushLatency = 0;
                let deliveryLatency = 0;

                const finish = () => {
                    if (failed || !writesComplete || !deliveryComplete) return;
                    broadcasts++;
                    if (collectLatencies) {
                        flushLatencies.push(flushLatency);
                        deliveryLatencies.push(deliveryLatency);
                    }
                    const now = process.hrtime.bigint();
                    if (now < deadline) {
                        setImmediate(next);
                    } else {
                        activeBroadcastRun = null;
                        resolve({
                            broadcasts,
                            elapsedSeconds: Number(now - start) / 1e9,
                            flushLatencies,
                            deliveryLatencies
                        });
                    }
                };

                activeBroadcastRun = {
                    round,
                    acknowledge(acknowledgedRound) {
                        if (acknowledgedRound !== round || deliveryComplete) return;
                        deliveryComplete = true;
                        deliveryLatency = Number(
                            process.hrtime.bigint() - roundStart
                        ) / 1e6;
                        finish();
                    }
                };

                const complete = (error) => {
                    if (failed) return;
                    if (error) {
                        failed = true;
                        activeBroadcastRun = null;
                        reject(error);
                        return;
                    }
                    remaining--;
                    if (remaining !== 0) return;
                    writesComplete = true;
                    flushLatency = Number(
                        process.hrtime.bigint() - roundStart
                    ) / 1e6;
                    finish();
                };
                for (const webSocket of sockets) sendOne(webSocket, complete);
            };
            next();
        });
    }

    process.on('message', async (message) => {
        const { id, command } = message;
        try {
            if (command === 'round-ack') {
                if (activeBroadcastRun) {
                    activeBroadcastRun.acknowledge(message.round);
                }
                return;
            }
            if (command === 'snapshot') {
                if (message.gc) collectGarbage();
                sendResponse(id, 'snapshot', {
                    memory: memorySnapshot(),
                    connections: sockets.size,
                    peakRss
                });
                return;
            }
            if (command === 'warmup') {
                const result = await runBroadcasts(message.duration, false);
                sendResponse(id, 'warmup', result);
                return;
            }
            if (command === 'measure') {
                collectGarbage();
                const startMemory = memorySnapshot();
                peakRss = startMemory.rss;
                const startCpu = process.cpuUsage();
                const result = await runBroadcasts(message.duration, true);
                const cpu = process.cpuUsage(startCpu);
                result.flushLatencies.sort((left, right) => left - right);
                result.deliveryLatencies.sort((left, right) => left - right);
                const deliveries = result.broadcasts * sockets.size;
                const cpuMicros = cpu.user + cpu.system;
                sendResponse(id, 'measure', {
                    broadcasts: result.broadcasts,
                    connections: sockets.size,
                    deliveries,
                    elapsedSeconds: result.elapsedSeconds,
                    broadcastsPerSecond: result.broadcasts / result.elapsedSeconds,
                    deliveriesPerSecond: deliveries / result.elapsedSeconds,
                    logicalMiBPerSecond:
                        deliveries * sourceSize / result.elapsedSeconds / 1024 / 1024,
                    wirePayloadMiBPerSecond:
                        deliveries * payloadBytes / result.elapsedSeconds / 1024 / 1024,
                    flushLatencyP50Ms: percentile(result.flushLatencies, 0.50),
                    flushLatencyP99Ms: percentile(result.flushLatencies, 0.99),
                    deliveryLatencyP50Ms: percentile(
                        result.deliveryLatencies,
                        0.50
                    ),
                    deliveryLatencyP99Ms: percentile(
                        result.deliveryLatencies,
                        0.99
                    ),
                    serverCpuPercent: cpuMicros / (result.elapsedSeconds * 10000),
                    millionDeliveriesPerCpuSecond:
                        deliveries / (cpuMicros / 1e6) / 1e6,
                    startRssBytes: startMemory.rss,
                    peakRssBytes: peakRss,
                    activePeakRssDeltaBytes: peakRss - startMemory.rss
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
        } catch (error) {
            process.send({ id, type: 'request-error', value: error.stack || String(error) });
        }
    });

    httpServer.listen(0, '127.0.0.1', () => {
        collectGarbage();
        const nativeSupport = implementation === 'ws' ? {
            bufferutilLoaded: Object.keys(require.cache).some((filename) =>
                filename.includes('/bufferutil/') && filename.endsWith('.node')),
            note: 'bufferutil is loaded but outbound server frames are not masked'
        } : null;
        process.send({
            type: 'ready',
            value: {
                port: httpServer.address().port,
                pid: process.pid,
                sourceSize,
                payloadBytes,
                binary,
                compressionRatio: payloadBytes / sourceSize,
                frameParts: preEncodedFrame.length,
                frameBytes: preEncodedFrame.reduce(
                    (total, part) => total + Buffer.byteLength(part),
                    0
                ),
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
