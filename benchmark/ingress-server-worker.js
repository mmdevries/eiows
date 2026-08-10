'use strict';

const http = require('node:http');
const https = require('node:https');
const fs = require('node:fs');
const path = require('node:path');

const [
    implementation,
    modulePath,
    binaryArgument,
    payloadBytesArgument,
    textConsumption = 'native',
    serverTextOutput = 'buffer',
    transport = 'tcp'
] =
    process.argv.slice(2);
const expectedBinary = binaryArgument === 'true';
const expectedPayloadBytes = Number(payloadBytesArgument);

if (!implementation || !modulePath ||
    !Number.isInteger(expectedPayloadBytes) || expectedPayloadBytes <= 0 ||
    !['native', 'string'].includes(textConsumption) ||
    !['buffer', 'string'].includes(serverTextOutput) ||
    !['tcp', 'tls'].includes(transport) ||
    typeof process.send !== 'function') {
    throw new Error('ingress-server-worker must be started by benchmark/ingress-run.js');
}

let sockets = new Set();
let messages = 0;
let bytes = 0;
let invalidMessage = null;
let peakRss = process.memoryUsage().rss;
let measurementStart = null;
let pendingFinish = null;

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

function finishMeasurement(id) {
    const elapsedNs = process.hrtime.bigint() - measurementStart.time;
    const cpu = process.cpuUsage(measurementStart.cpu);
    const elapsedSeconds = Number(elapsedNs) / 1e9;
    const cpuMicros = cpu.user + cpu.system;
    sendResponse(id, 'finish-measurement', {
        messages,
        bytes,
        elapsedSeconds,
        messagesPerSecond: messages / elapsedSeconds,
        serverCpuPercent: cpuMicros / (elapsedSeconds * 10000),
        millionMessagesPerCpuSecond: messages / (cpuMicros / 1e6) / 1e6,
        startRssBytes: measurementStart.memory.rss,
        peakRssBytes: peakRss,
        activePeakRssDeltaBytes: peakRss - measurementStart.memory.rss,
        invalidMessage
    });
    measurementStart = null;
    pendingFinish = null;
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
        perMessageDeflate: false,
        textAsString: implementation === 'current' && serverTextOutput === 'string'
    });
    const httpServer = transport === 'tls'
        ? https.createServer({
            key: fs.readFileSync(path.join(__dirname, '..', 'test', 'fixtures', 'key.pem')),
            cert: fs.readFileSync(path.join(__dirname, '..', 'test', 'fixtures', 'cert.pem'))
        })
        : http.createServer();

    httpServer.on('upgrade', (request, socket, head) => {
        websocketServer.handleUpgrade(request, socket, head, (webSocket) => {
            sockets.add(webSocket);
            webSocket.on('error', () => {});
            webSocket.on('close', () => sockets.delete(webSocket));
            webSocket.on('message', (data, isBinary) => {
                messages++;
                const consumed = !expectedBinary && textConsumption === 'string' &&
                    typeof data !== 'string'
                    ? data.toString('utf8')
                    : data;
                const length = consumed.byteLength === undefined
                    ? Buffer.byteLength(consumed)
                    : consumed.byteLength;
                bytes += length;
                if (!invalidMessage &&
                    (isBinary !== expectedBinary || length !== expectedPayloadBytes)) {
                    invalidMessage = `expected ${expectedBinary ? 'binary' : 'text'} ` +
                        `${expectedPayloadBytes} B, received ${isBinary ? 'binary' : 'text'} ` +
                        `${length} B`;
                }
                if (pendingFinish && messages >= pendingFinish.expectedMessages) {
                    finishMeasurement(pendingFinish.id);
                }
            });
        });
    });

    const peakTimer = setInterval(() => {
        peakRss = Math.max(peakRss, process.memoryUsage().rss);
    }, 10);
    peakTimer.unref();

    process.on('message', (message) => {
        const { id, command } = message;
        try {
            if (command === 'reset') {
                messages = 0;
                bytes = 0;
                invalidMessage = null;
                sendResponse(id, 'reset', true);
                return;
            }
            if (command === 'snapshot') {
                sendResponse(id, 'snapshot', { messages, bytes, invalidMessage });
                return;
            }
            if (command === 'begin-measurement') {
                collectGarbage();
                const memory = memorySnapshot();
                peakRss = memory.rss;
                messages = 0;
                bytes = 0;
                invalidMessage = null;
                measurementStart = {
                    cpu: process.cpuUsage(),
                    time: process.hrtime.bigint(),
                    memory
                };
                sendResponse(id, 'begin-measurement', memory);
                return;
            }
            if (command === 'finish-measurement') {
                if (!measurementStart) {
                    throw new Error('measurement has not started');
                }
                if (messages >= message.expectedMessages) {
                    finishMeasurement(id);
                } else {
                    pendingFinish = { id, expectedMessages: message.expectedMessages };
                }
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
