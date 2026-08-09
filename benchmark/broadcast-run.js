'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { fork } = require('node:child_process');
const { once } = require('node:events');
const { performance } = require('node:perf_hooks');

const previousNoBufferUtil = process.env.WS_NO_BUFFER_UTIL;
process.env.WS_NO_BUFFER_UTIL = '1';
const WebSocket = require('ws');
if (previousNoBufferUtil === undefined) {
    delete process.env.WS_NO_BUFFER_UTIL;
} else {
    process.env.WS_NO_BUFFER_UTIL = previousNoBufferUtil;
}

const benchmarkDirectory = __dirname;
const repositoryDirectory = path.join(benchmarkDirectory, '..');
const workerPath = path.join(benchmarkDirectory, 'broadcast-server-worker.js');
const OPEN_BATCH_SIZE = 100;
const variants = [
    { id: 'current-sendFrame', implementation: 'current', sendMode: 'sendFrame' },
    { id: 'current-send', implementation: 'current', sendMode: 'send' },
    { id: 'eiows-9.2.0-send', implementation: 'eiows-9.2.0', sendMode: 'send' },
    { id: 'ws-sendFrame', implementation: 'ws', sendMode: 'sendFrame' },
    { id: 'ws-send', implementation: 'ws', sendMode: 'send' }
];

function parsePositiveNumber(value, option) {
    const number = Number(value);
    if (!Number.isFinite(number) || number <= 0) {
        throw new TypeError(`${option} must be a positive number`);
    }
    return number;
}

function parsePositiveInteger(value, option) {
    const number = parsePositiveNumber(value, option);
    if (!Number.isInteger(number)) {
        throw new TypeError(`${option} must be an integer`);
    }
    return number;
}

function parseArguments(argv) {
    const options = {
        iterations: 2,
        duration: 3,
        warmup: 1,
        connections: 100,
        payloads: [1024, 16 * 1024, 320 * 1024],
        dataModes: ['text', 'app-deflate'],
        output: null
    };
    for (let index = 0; index < argv.length; index++) {
        const option = argv[index];
        const value = argv[++index];
        if (value === undefined) throw new TypeError(`missing value for ${option}`);
        if (option === '--iterations') {
            options.iterations = parsePositiveInteger(value, option);
        } else if (option === '--duration') {
            options.duration = parsePositiveNumber(value, option);
        } else if (option === '--warmup') {
            options.warmup = parsePositiveNumber(value, option);
        } else if (option === '--connections') {
            options.connections = parsePositiveInteger(value, option);
        } else if (option === '--payloads') {
            options.payloads = value.split(',').map((entry) =>
                parsePositiveInteger(entry, option));
        } else if (option === '--data-modes') {
            options.dataModes = value.split(',').filter(Boolean);
        } else if (option === '--output') {
            options.output = path.resolve(value);
        } else {
            throw new TypeError(`unknown option: ${option}`);
        }
    }
    if (!options.dataModes.length ||
        options.dataModes.some((mode) => !['text', 'app-deflate'].includes(mode))) {
        throw new TypeError('--data-modes supports text and app-deflate');
    }
    return options;
}

function modulePathFor(implementation) {
    if (implementation === 'current') return repositoryDirectory;
    return path.join(benchmarkDirectory, 'node_modules', implementation);
}

class BroadcastServer {
    constructor(variant, dataMode, sourceSize) {
        this.variant = variant;
        this.nextRequestId = 1;
        this.pending = new Map();
        this.ready = new Promise((resolve, reject) => {
            this.resolveReady = resolve;
            this.rejectReady = reject;
        });
        const serverEnvironment = { ...process.env };
        delete serverEnvironment.WS_NO_BUFFER_UTIL;
        delete serverEnvironment.WS_NO_UTF_8_VALIDATE;
        this.child = fork(
            workerPath,
            [
                variant.implementation,
                modulePathFor(variant.implementation),
                variant.sendMode,
                dataMode,
                String(sourceSize)
            ],
            {
                stdio: ['ignore', 'inherit', 'inherit', 'ipc'],
                execArgv: ['--expose-gc'],
                env: serverEnvironment
            }
        );
        this.child.on('message', (message) => this.onMessage(message));
        this.child.once('error', (error) => this.fail(error));
        this.child.once('exit', (code, signal) => {
            if (code || signal) {
                this.fail(new Error(
                    `${variant.id} server exited with code ${code}, signal ${signal}`
                ));
            }
        });
    }

    onMessage(message) {
        if (message.type === 'ready') {
            if (this.variant.implementation === 'ws' &&
                !message.value.nativeSupport.bufferutilLoaded) {
                this.fail(new Error('ws server did not load bufferutil'));
                return;
            }
            this.resolveReady(message.value);
            return;
        }
        if (message.type === 'fatal') {
            this.fail(new Error(message.value));
            return;
        }
        const pending = this.pending.get(message.id);
        if (!pending) return;
        this.pending.delete(message.id);
        if (message.type === 'request-error') {
            pending.reject(new Error(message.value));
        } else {
            pending.resolve(message.value);
        }
    }

    fail(error) {
        this.rejectReady(error);
        for (const pending of this.pending.values()) pending.reject(error);
        this.pending.clear();
    }

    request(command, options = {}) {
        const id = this.nextRequestId++;
        return new Promise((resolve, reject) => {
            this.pending.set(id, { resolve, reject });
            this.child.send({ id, command, ...options }, (error) => {
                if (!error) return;
                this.pending.delete(id);
                reject(error);
            });
        });
    }

    acknowledgeRound(round) {
        this.child.send({ command: 'round-ack', round });
    }

    async shutdown() {
        if (!this.child.connected) return;
        await this.request('shutdown');
        if (this.child.exitCode === null) await once(this.child, 'exit');
    }

    terminate() {
        if (this.child.exitCode === null) this.child.kill('SIGTERM');
    }
}

function openClient(url, ready) {
    return new Promise((resolve, reject) => {
        const client = new WebSocket(url, { perMessageDeflate: false });
        client.benchmarkMessages = 0;
        client.benchmarkValidated = false;
        client.benchmarkError = null;
        client.benchmarkPhaseMessages = 0;
        client.benchmarkOnMessage = null;
        const onError = (error) => {
            client.removeListener('open', onOpen);
            reject(error);
        };
        const onOpen = () => {
            client.removeListener('error', onError);
            client.on('error', () => {});
            client.on('message', (data, isBinary) => {
                if (!client.benchmarkValidated) {
                    if (isBinary !== ready.binary) {
                        client.benchmarkError = new Error(
                            `expected ${ready.binary ? 'binary' : 'text'} frame`
                        );
                        return;
                    }
                    if (data.byteLength !== ready.payloadBytes) {
                        client.benchmarkError = new Error(
                            `expected ${ready.payloadBytes} payload bytes, got ` +
                            data.byteLength
                        );
                        return;
                    }
                    client.benchmarkValidated = true;
                }
                client.benchmarkMessages++;
                client.benchmarkPhaseMessages++;
                if (client.benchmarkOnMessage) {
                    client.benchmarkOnMessage(client.benchmarkPhaseMessages);
                }
            });
            resolve(client);
        };
        client.once('error', onError);
        client.once('open', onOpen);
    });
}

async function addClients(clients, url, count, ready) {
    let remaining = count;
    while (remaining > 0) {
        const batchSize = Math.min(OPEN_BATCH_SIZE, remaining);
        const batch = await Promise.all(
            Array.from({ length: batchSize }, () => openClient(url, ready))
        );
        clients.push(...batch);
        remaining -= batchSize;
    }
}

async function runFlowControlledPhase(server, clients, command, duration) {
    const arrivals = new Map();
    for (const client of clients) {
        client.benchmarkPhaseMessages = 0;
        client.benchmarkOnMessage = (round) => {
            const count = (arrivals.get(round) || 0) + 1;
            if (count === clients.length) {
                arrivals.delete(round);
                server.acknowledgeRound(round);
            } else {
                arrivals.set(round, count);
            }
        };
    }
    let result;
    try {
        result = await server.request(command, { duration });
    } finally {
        for (const client of clients) client.benchmarkOnMessage = null;
    }
    const errorClient = clients.find((client) => client.benchmarkError);
    if (errorClient) throw errorClient.benchmarkError;
    for (const client of clients) {
        if (!client.benchmarkValidated) {
            throw new Error('a client did not validate a broadcast payload');
        }
        if (client.benchmarkPhaseMessages !== result.broadcasts) {
            throw new Error(
                `expected exactly ${result.broadcasts} phase messages, got ` +
                client.benchmarkPhaseMessages
            );
        }
    }
    return result;
}

async function closeClients(clients) {
    const closed = clients.map((client) => {
        if (client.readyState === WebSocket.CLOSED) return Promise.resolve();
        const result = once(client, 'close');
        client.terminate();
        return result;
    });
    await Promise.all(closed);
    clients.length = 0;
}

async function runCase(variant, dataMode, sourceSize, options) {
    const server = new BroadcastServer(variant, dataMode, sourceSize);
    const clients = [];
    try {
        const ready = await server.ready;
        await addClients(
            clients,
            `ws://127.0.0.1:${ready.port}`,
            options.connections,
            ready
        );
        const snapshot = await server.request('snapshot', { gc: true });
        if (snapshot.connections !== options.connections) {
            throw new Error(
                `expected ${options.connections} server connections, got ` +
                snapshot.connections
            );
        }
        await runFlowControlledPhase(
            server,
            clients,
            'warmup',
            options.warmup
        );
        const measurement = await runFlowControlledPhase(
            server,
            clients,
            'measure',
            options.duration
        );
        return {
            variant: variant.id,
            implementation: variant.implementation,
            sendMode: variant.sendMode,
            dataMode,
            sourceSize,
            wirePayloadBytes: ready.payloadBytes,
            compressionRatio: ready.compressionRatio,
            frameBytes: ready.frameBytes,
            ...measurement
        };
    } finally {
        await closeClients(clients);
        try {
            await server.shutdown();
        } catch {
            server.terminate();
        }
    }
}

function median(values) {
    const sorted = [...values].sort((left, right) => left - right);
    const middle = Math.floor(sorted.length / 2);
    return sorted.length % 2
        ? sorted[middle]
        : (sorted[middle - 1] + sorted[middle]) / 2;
}

function formatNumber(value, digits = 0) {
    return value.toLocaleString('en-US', {
        minimumFractionDigits: digits,
        maximumFractionDigits: digits
    });
}

function aggregate(results, options) {
    const rows = [];
    for (const sourceSize of options.payloads) {
        for (const dataMode of options.dataModes) {
            for (const variant of variants) {
                const matching = results.filter((result) =>
                    result.sourceSize === sourceSize &&
                    result.dataMode === dataMode &&
                    result.variant === variant.id);
                const value = (property) => median(
                    matching.map((result) => result[property])
                );
                rows.push({
                    variant: variant.id,
                    source: `${formatNumber(sourceSize)} B`,
                    data: dataMode,
                    'wire payload B': formatNumber(value('wirePayloadBytes')),
                    'broadcasts/s': formatNumber(value('broadcastsPerSecond')),
                    'deliveries/s': formatNumber(value('deliveriesPerSecond')),
                    'logical MiB/s': formatNumber(value('logicalMiBPerSecond'), 1),
                    'wire MiB/s': formatNumber(value('wirePayloadMiBPerSecond'), 1),
                    'flush p50 ms': formatNumber(value('flushLatencyP50Ms'), 3),
                    'flush p99 ms': formatNumber(value('flushLatencyP99Ms'), 3),
                    'delivery p50 ms': formatNumber(
                        value('deliveryLatencyP50Ms'),
                        3
                    ),
                    'delivery p99 ms': formatNumber(
                        value('deliveryLatencyP99Ms'),
                        3
                    ),
                    'server CPU %': formatNumber(value('serverCpuPercent'), 1),
                    'M deliveries/CPU-s': formatNumber(
                        value('millionDeliveriesPerCpuSecond'),
                        3
                    ),
                    'peak RSS +MiB': formatNumber(
                        value('activePeakRssDeltaBytes') / 1024 / 1024,
                        1
                    )
                });
            }
        }
    }
    return rows;
}

function currentSpeedups(results, options) {
    const rows = [];
    for (const sourceSize of options.payloads) {
        for (const dataMode of options.dataModes) {
            const frameResults = results.filter((result) =>
                result.sourceSize === sourceSize &&
                result.dataMode === dataMode &&
                result.variant === 'current-sendFrame');
            const sendResults = results.filter((result) =>
                result.sourceSize === sourceSize &&
                result.dataMode === dataMode &&
                result.variant === 'current-send');
            const frameThroughput = median(frameResults.map((result) =>
                result.deliveriesPerSecond));
            const sendThroughput = median(sendResults.map((result) =>
                result.deliveriesPerSecond));
            const frameEfficiency = median(frameResults.map((result) =>
                result.millionDeliveriesPerCpuSecond));
            const sendEfficiency = median(sendResults.map((result) =>
                result.millionDeliveriesPerCpuSecond));
            rows.push({
                source: `${formatNumber(sourceSize)} B`,
                data: dataMode,
                'sendFrame throughput gain':
                    `${formatNumber((frameThroughput / sendThroughput - 1) * 100, 1)}%`,
                'sendFrame CPU-efficiency gain':
                    `${formatNumber((frameEfficiency / sendEfficiency - 1) * 100, 1)}%`
            });
        }
    }
    return rows;
}

async function main() {
    const options = parseArguments(process.argv.slice(2));
    for (const dependency of ['eiows-9.2.0', 'ws', 'bufferutil']) {
        if (!fs.existsSync(path.join(benchmarkDirectory, 'node_modules', dependency))) {
            throw new Error(
                `missing benchmark dependency ${dependency}; ` +
                'run "npm install --prefix benchmark" first'
            );
        }
    }
    const startedAt = new Date().toISOString();
    const results = [];
    console.log(
        'Broadcast benchmark: permessage-deflate off; app-deflate payloads are ' +
        'compressed once before measurement'
    );
    console.log('Configuration:', JSON.stringify(options));

    for (const sourceSize of options.payloads) {
        for (const dataMode of options.dataModes) {
            for (let iteration = 0; iteration < options.iterations; iteration++) {
                const rotated = variants.map((_, index) =>
                    variants[(index + iteration) % variants.length]);
                for (const variant of rotated) {
                    const label = `${variant.id}, ${sourceSize} B, ${dataMode}, ` +
                        `iteration ${iteration + 1}/${options.iterations}`;
                    process.stdout.write(`  ${label} ... `);
                    const start = performance.now();
                    const result = await runCase(
                        variant,
                        dataMode,
                        sourceSize,
                        options
                    );
                    results.push(result);
                    console.log(
                        `${formatNumber(result.deliveriesPerSecond)} deliveries/s, ` +
                        `${formatNumber(result.serverCpuPercent, 1)}% CPU ` +
                        `(${formatNumber((performance.now() - start) / 1000, 1)} s)`
                    );
                }
            }
        }
    }

    console.log('\nBroadcast medians');
    console.table(aggregate(results, options));
    console.log('\nCurrent eiows: sendFrame versus send');
    console.table(currentSpeedups(results, options));

    const report = {
        metadata: {
            startedAt,
            finishedAt: new Date().toISOString(),
            node: process.version,
            platform: process.platform,
            architecture: process.arch,
            cpus: os.cpus().map((cpu) => cpu.model),
            totalMemoryBytes: os.totalmem(),
            wsNativeSupport: {
                bufferutil: require('bufferutil/package.json').version,
                note: 'loaded in ws server; outbound server frames are not masked'
            },
            applicationCompression: {
                algorithm: 'deflateRaw',
                timing: 'once before warmup; compression CPU excluded'
            },
            options
        },
        results
    };
    if (options.output) {
        fs.mkdirSync(path.dirname(options.output), { recursive: true });
        fs.writeFileSync(options.output, `${JSON.stringify(report, null, 2)}\n`);
        console.log(`\nRaw JSON written to ${options.output}`);
    }
}

main().catch((error) => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
