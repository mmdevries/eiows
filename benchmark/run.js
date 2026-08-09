'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { isUtf8 } = require('node:buffer');
const { fork } = require('node:child_process');
const { once } = require('node:events');
const { performance } = require('node:perf_hooks');

// Keep the load generator identical for every candidate. The ws server runs in
// a separate process and explicitly enables its native addons.
const previousNoBufferUtil = process.env.WS_NO_BUFFER_UTIL;
process.env.WS_NO_BUFFER_UTIL = '1';
const WebSocket = require('ws');
if (previousNoBufferUtil === undefined) {
    delete process.env.WS_NO_BUFFER_UTIL;
} else {
    process.env.WS_NO_BUFFER_UTIL = previousNoBufferUtil;
}
const bufferutil = require('bufferutil');
const utf8Validate = require('utf-8-validate');

const benchmarkDirectory = __dirname;
const repositoryDirectory = path.join(benchmarkDirectory, '..');
const workerPath = path.join(benchmarkDirectory, 'server-worker.js');
const LATENCY_SAMPLE_STRIDE = 16;
const OPEN_BATCH_SIZE = 100;

function verifyNativeAddons() {
    if (typeof bufferutil.mask !== 'function' ||
        typeof bufferutil.unmask !== 'function') {
        throw new TypeError('bufferutil native addon did not load correctly');
    }
    if (typeof utf8Validate !== 'function' ||
        utf8Validate(Buffer.from('valid UTF-8')) !== true) {
        throw new TypeError('utf-8-validate native addon did not load correctly');
    }
}

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
        implementations: ['current', 'eiows-9.2.0', 'ws'],
        iterations: 2,
        duration: 3,
        warmup: 1,
        connections: 100,
        inflight: 1,
        payloads: [1024, 16 * 1024, 320 * 1024],
        messageTypes: ['text', 'binary'],
        compression: ['off'],
        memoryConnections: 2000,
        memoryStep: 500,
        output: null
    };

    for (let index = 0; index < argv.length; index++) {
        const option = argv[index];
        const value = argv[++index];
        if (value === undefined) throw new TypeError(`missing value for ${option}`);
        if (option === '--implementations') {
            options.implementations = value.split(',').filter(Boolean);
        } else if (option === '--iterations') {
            options.iterations = parsePositiveInteger(value, option);
        } else if (option === '--duration') {
            options.duration = parsePositiveNumber(value, option);
        } else if (option === '--warmup') {
            options.warmup = parsePositiveNumber(value, option);
        } else if (option === '--connections') {
            options.connections = parsePositiveInteger(value, option);
        } else if (option === '--inflight') {
            options.inflight = parsePositiveInteger(value, option);
        } else if (option === '--payloads') {
            options.payloads = value.split(',').map((entry) =>
                parsePositiveInteger(entry, option));
        } else if (option === '--message-types') {
            options.messageTypes = value.split(',').filter(Boolean);
        } else if (option === '--compression') {
            options.compression = value.split(',').filter(Boolean);
        } else if (option === '--memory-connections') {
            options.memoryConnections = parsePositiveInteger(value, option);
        } else if (option === '--memory-step') {
            options.memoryStep = parsePositiveInteger(value, option);
        } else if (option === '--output') {
            options.output = path.resolve(value);
        } else {
            throw new TypeError(`unknown option: ${option}`);
        }
    }

    if (!options.implementations.length) {
        throw new TypeError('--implementations cannot be empty');
    }
    const supported = new Set(['current', 'eiows-9.2.0', 'ws']);
    for (const implementation of options.implementations) {
        if (!supported.has(implementation)) {
            throw new TypeError(`unsupported implementation: ${implementation}`);
        }
    }
    if (options.payloads.some((size) => size < 17)) {
        throw new TypeError('payload sizes must be at least 17 bytes');
    }
    if (options.messageTypes.some((type) => !['text', 'binary'].includes(type))) {
        throw new TypeError('--message-types supports only text and binary');
    }
    if (options.compression.some((value) => !['off', 'on'].includes(value))) {
        throw new TypeError('--compression supports only off and on');
    }
    if (!options.messageTypes.length || !options.compression.length) {
        throw new TypeError('message-types and compression cannot be empty');
    }
    return options;
}

function modulePathFor(implementation) {
    if (implementation === 'current') return repositoryDirectory;
    return path.join(benchmarkDirectory, 'node_modules', implementation);
}

class BenchmarkServer {
    constructor(implementation, compression) {
        this.implementation = implementation;
        this.nextRequestId = 1;
        this.pending = new Map();
        this.ready = new Promise((resolve, reject) => {
            this.resolveReady = resolve;
            this.rejectReady = reject;
        });
        const serverEnvironment = { ...process.env };
        delete serverEnvironment.WS_NO_BUFFER_UTIL;
        delete serverEnvironment.WS_NO_UTF_8_VALIDATE;
        const execArgv = ['--expose-gc'];
        const cpuProfileDirectory = process.env.EIOWS_BENCHMARK_CPU_PROF_DIR;
        if (cpuProfileDirectory && implementation === 'current') {
            fs.mkdirSync(cpuProfileDirectory, { recursive: true });
            execArgv.push('--cpu-prof', `--cpu-prof-dir=${cpuProfileDirectory}`);
        }
        this.child = fork(
            workerPath,
            [implementation, modulePathFor(implementation), compression],
            {
                stdio: ['ignore', 'inherit', 'inherit', 'ipc'],
                execArgv,
                env: serverEnvironment
            }
        );
        this.child.on('message', (message) => this.onMessage(message));
        this.child.once('error', (error) => this.fail(error));
        this.child.once('exit', (code, signal) => {
            if (code || signal) {
                this.fail(new Error(
                    `${implementation} server exited with code ${code}, signal ${signal}`
                ));
            }
        });
    }

    onMessage(message) {
        if (message.type === 'ready') {
            if (this.implementation === 'ws' &&
                !message.value.nativeSupport.bufferutilLoaded) {
                this.fail(new Error(
                    'ws server did not load the bufferutil native addon'
                ));
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
        pending.resolve(message.value);
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

    async shutdown() {
        if (!this.child.connected) return;
        await this.request('shutdown');
        if (this.child.exitCode === null) await once(this.child, 'exit');
    }

    terminate() {
        if (this.child.exitCode === null) this.child.kill('SIGTERM');
    }
}

function openClient(url, compression) {
    return new Promise((resolve, reject) => {
        const client = new WebSocket(url, {
            perMessageDeflate: compression === 'on' ? {
                threshold: 0,
                clientNoContextTakeover: true,
                serverNoContextTakeover: true
            } : false
        });
        const onError = (error) => {
            client.removeListener('open', onOpen);
            reject(error);
        };
        const onOpen = () => {
            client.removeListener('error', onError);
            client.on('error', () => {});
            const negotiatedCompression = client.extensions
                .split(',')
                .some((extension) => extension.trim() === 'permessage-deflate');
            if (negotiatedCompression !== (compression === 'on')) {
                client.terminate();
                reject(new Error(
                    `expected permessage-deflate ${compression}, got ` +
                    `${client.extensions || 'no extensions'}`
                ));
                return;
            }
            resolve(client);
        };
        client.once('error', onError);
        client.once('open', onOpen);
    });
}

async function addClients(clients, url, count, compression) {
    let remaining = count;
    while (remaining > 0) {
        const batchSize = Math.min(OPEN_BATCH_SIZE, remaining);
        const batch = await Promise.all(
            Array.from({ length: batchSize }, () => openClient(url, compression))
        );
        clients.push(...batch);
        remaining -= batchSize;
    }
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

function percentile(sortedValues, percentileValue) {
    if (!sortedValues.length) return 0;
    const index = Math.min(
        sortedValues.length - 1,
        Math.ceil(percentileValue * sortedValues.length) - 1
    );
    return sortedValues[index];
}

class EchoLoad {
    constructor(clients, payloadSize, inflight, messageType, compression) {
        this.clients = clients;
        this.payloadSize = payloadSize;
        this.inflight = inflight;
        this.messageType = messageType;
        this.compression = compression;
        this.phase = null;
        for (const client of clients) {
            client.on('message', (data, isBinary) =>
                this.onMessage(client, data, isBinary));
        }
    }

    send(client, buffer, phase) {
        const sequence = phase.sequence++;
        const sampled = sequence % LATENCY_SAMPLE_STRIDE === 0;
        if (this.messageType === 'binary') {
            buffer[0] = sampled ? 1 : 0;
            buffer.writeBigUInt64BE(process.hrtime.bigint(), 1);
        } else {
            buffer[0] = sampled ? 0x31 : 0x30;
            const timestamp = process.hrtime.bigint().toString(16).padStart(16, '0');
            buffer.write(timestamp, 1, 16, 'ascii');
        }
        phase.outstanding++;
        client.send(buffer, {
            binary: this.messageType === 'binary',
            compress: this.compression === 'on'
        });
    }

    onMessage(client, data, isBinary) {
        const phase = this.phase;
        if (!phase) return;
        if (isBinary !== (this.messageType === 'binary')) {
            phase.stopped = true;
            phase.reject(new Error(
                `expected a ${this.messageType} echo, received ` +
                `${isBinary ? 'binary' : 'text'}`
            ));
            return;
        }
        const now = process.hrtime.bigint();
        phase.outstanding--;
        if (phase.measure && now <= phase.deadline) {
            phase.messages++;
            const sampled = this.messageType === 'binary'
                ? data[0] === 1
                : data[0] === 0x31;
            if (sampled) {
                const sent = this.messageType === 'binary'
                    ? data.readBigUInt64BE(1)
                    : BigInt(`0x${data.toString('ascii', 1, 17)}`);
                phase.latencies.push(Number(now - sent) / 1e6);
            }
        }
        if (!phase.stopped && now < phase.deadline) {
            this.send(client, data, phase);
        } else if (phase.outstanding === 0) {
            phase.resolve();
        }
    }

    async run(durationSeconds, measure) {
        if (this.phase) throw new Error('an echo phase is already active');
        const durationNs = BigInt(Math.round(durationSeconds * 1e9));
        const start = process.hrtime.bigint();
        const phase = {
            deadline: start + durationNs,
            stopped: false,
            measure,
            messages: 0,
            latencies: [],
            sequence: 0,
            outstanding: 0,
            resolve: null,
            reject: null
        };
        this.phase = phase;
        const drained = new Promise((resolve, reject) => {
            phase.resolve = resolve;
            phase.reject = reject;
        });
        const stopTimer = setTimeout(() => {
            phase.stopped = true;
            if (phase.outstanding === 0) phase.resolve();
        }, durationSeconds * 1000);
        const timeout = setTimeout(() => {
            phase.reject(new Error('timed out while draining echo requests'));
        }, durationSeconds * 1000 + 10000);

        for (const client of this.clients) {
            for (let index = 0; index < this.inflight; index++) {
                this.send(client, Buffer.alloc(this.payloadSize, 0x61), phase);
            }
        }

        try {
            await drained;
        } finally {
            clearTimeout(stopTimer);
            clearTimeout(timeout);
            this.phase = null;
        }

        phase.latencies.sort((left, right) => left - right);
        return {
            messages: phase.messages,
            durationSeconds,
            messagesPerSecond: phase.messages / durationSeconds,
            bidirectionalMiBPerSecond:
                phase.messages * this.payloadSize * 2 / durationSeconds / 1024 / 1024,
            latencySamples: phase.latencies.length,
            latencyP50Ms: percentile(phase.latencies, 0.50),
            latencyP95Ms: percentile(phase.latencies, 0.95),
            latencyP99Ms: percentile(phase.latencies, 0.99)
        };
    }
}

async function runPerformanceCase(
    implementation,
    payloadSize,
    messageType,
    compression,
    options
) {
    const server = new BenchmarkServer(implementation, compression);
    const clients = [];
    try {
        const ready = await server.ready;
        const url = `ws://127.0.0.1:${ready.port}`;
        await addClients(clients, url, options.connections, compression);
        const idle = await server.request('snapshot', { gc: true });
        const load = new EchoLoad(
            clients,
            payloadSize,
            options.inflight,
            messageType,
            compression
        );
        await load.run(options.warmup, false);
        const startMemory = await server.request('begin-measurement');
        const loadResult = await load.run(options.duration, true);
        const serverResult = await server.request('end-measurement');
        const cpuMicros = serverResult.cpuUserMicros + serverResult.cpuSystemMicros;
        return {
            implementation,
            payloadSize,
            messageType,
            compression,
            ...loadResult,
            serverMessages: serverResult.messages,
            serverCpuPercent: cpuMicros / (serverResult.elapsedMs * 10),
            thousandRoundTripsPerCpuSecond:
                loadResult.messages / (cpuMicros / 1e6) / 1000,
            baselineRssBytes: ready.memory.rss,
            idleRssBytes: idle.memory.rss,
            idleRssDeltaBytes: idle.memory.rss - ready.memory.rss,
            measurementStartRssBytes: startMemory.rss,
            peakRssBytes: serverResult.peakRss,
            activePeakRssDeltaBytes: serverResult.peakRss - startMemory.rss
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

function regression(points) {
    const count = points.length;
    const meanX = points.reduce((sum, point) => sum + point.connections, 0) / count;
    const meanY = points.reduce((sum, point) => sum + point.rssBytes, 0) / count;
    let covariance = 0;
    let varianceX = 0;
    let totalVarianceY = 0;
    for (const point of points) {
        const dx = point.connections - meanX;
        const dy = point.rssBytes - meanY;
        covariance += dx * dy;
        varianceX += dx * dx;
        totalVarianceY += dy * dy;
    }
    const slope = varianceX ? covariance / varianceX : 0;
    const intercept = meanY - slope * meanX;
    let residualVariance = 0;
    for (const point of points) {
        const residual = point.rssBytes - (intercept + slope * point.connections);
        residualVariance += residual * residual;
    }
    return {
        bytesPerConnection: slope,
        rSquared: totalVarianceY ? 1 - residualVariance / totalVarianceY : 1
    };
}

async function runMemoryCase(implementation, compression, options) {
    const server = new BenchmarkServer(implementation, compression);
    const clients = [];
    try {
        const ready = await server.ready;
        const url = `ws://127.0.0.1:${ready.port}`;
        const baseline = await server.request('snapshot', { gc: true });
        const points = [{ connections: 0, rssBytes: baseline.memory.rss }];
        let target = Math.min(options.memoryStep, options.memoryConnections);
        while (clients.length < options.memoryConnections) {
            await addClients(
                clients,
                url,
                target - clients.length,
                compression
            );
            await new Promise((resolve) => setTimeout(resolve, 150));
            const snapshot = await server.request('snapshot', { gc: true });
            if (snapshot.connections !== clients.length) {
                throw new Error(
                    `${implementation}: expected ${clients.length} server connections, ` +
                    `observed ${snapshot.connections}`
                );
            }
            points.push({
                connections: snapshot.connections,
                rssBytes: snapshot.memory.rss,
                heapUsedBytes: snapshot.memory.heapUsed,
                externalBytes: snapshot.memory.external
            });
            target = Math.min(
                options.memoryConnections,
                target + options.memoryStep
            );
        }
        const fit = regression(points);
        return {
            implementation,
            compression,
            points,
            baselineRssBytes: points[0].rssBytes,
            finalRssBytes: points.at(-1).rssBytes,
            rssDeltaBytes: points.at(-1).rssBytes - points[0].rssBytes,
            bytesPerConnection: fit.bytesPerConnection,
            rSquared: fit.rSquared
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

function aggregatePerformance(
    results,
    implementations,
    payloads,
    messageTypes,
    compressionValues
) {
    const rows = [];
    for (const payloadSize of payloads) {
        for (const messageType of messageTypes) {
            for (const compression of compressionValues) {
                for (const implementation of implementations) {
                    const matching = results.filter((result) =>
                        result.implementation === implementation &&
                        result.payloadSize === payloadSize &&
                        result.messageType === messageType &&
                        result.compression === compression);
                    const value = (property) => median(
                        matching.map((result) => result[property])
                    );
                    rows.push({
                        implementation,
                        payload: `${formatNumber(payloadSize)} B`,
                        type: messageType,
                        compression,
                        'round trips/s': formatNumber(value('messagesPerSecond')),
                        'bidirectional MiB/s': formatNumber(
                            value('bidirectionalMiBPerSecond'),
                            1
                        ),
                        'p50 ms': formatNumber(value('latencyP50Ms'), 3),
                        'p95 ms': formatNumber(value('latencyP95Ms'), 3),
                        'p99 ms': formatNumber(value('latencyP99Ms'), 3),
                        'server CPU %': formatNumber(value('serverCpuPercent'), 1),
                        'k round trips/CPU-s': formatNumber(
                            value('thousandRoundTripsPerCpuSecond'),
                            1
                        ),
                        'active peak RSS +MiB': formatNumber(
                            value('activePeakRssDeltaBytes') / 1024 / 1024,
                            1
                        )
                    });
                }
            }
        }
    }
    return rows;
}

function memoryTable(results) {
    return results.map((result) => ({
        implementation: result.implementation,
        compression: result.compression,
        connections: result.points.at(-1).connections,
        'baseline RSS MiB': formatNumber(result.baselineRssBytes / 1024 / 1024, 1),
        'final RSS MiB': formatNumber(result.finalRssBytes / 1024 / 1024, 1),
        'RSS delta MiB': formatNumber(result.rssDeltaBytes / 1024 / 1024, 1),
        'KiB/connection': formatNumber(result.bytesPerConnection / 1024, 2),
        'regression R²': formatNumber(result.rSquared, 3)
    }));
}

async function main() {
    verifyNativeAddons();
    const options = parseArguments(process.argv.slice(2));
    const missing = options.implementations
        .filter((implementation) => implementation !== 'current')
        .filter((implementation) => !fs.existsSync(modulePathFor(implementation)));
    if (missing.length) {
        throw new Error(
            `missing benchmark dependencies (${missing.join(', ')}); ` +
            'run "npm install --prefix benchmark" first'
        );
    }

    const startedAt = new Date().toISOString();
    const performanceResults = [];
    const memoryResults = [];
    console.log(
        'ws server native support: bufferutil enabled; utf-8-validate loaded ' +
        '(Node uses built-in Buffer.isUtf8); load generator uses JS masking'
    );
    console.log('Configuration:', JSON.stringify(options));
    console.log('Performance benchmark (server process is isolated)');

    for (const payloadSize of options.payloads) {
        for (const messageType of options.messageTypes) {
            for (const compression of options.compression) {
                for (let iteration = 0; iteration < options.iterations; iteration++) {
                    const rotated = options.implementations.map((_, index) =>
                        options.implementations[
                            (index + iteration) % options.implementations.length
                        ]);
                    for (const implementation of rotated) {
                        const label = `${implementation}, ${payloadSize} B, ` +
                            `${messageType}, compression ${compression}, ` +
                            `iteration ${iteration + 1}/${options.iterations}`;
                        process.stdout.write(`  ${label} ... `);
                        const start = performance.now();
                        const result = await runPerformanceCase(
                            implementation,
                            payloadSize,
                            messageType,
                            compression,
                            options
                        );
                        performanceResults.push(result);
                        console.log(
                            `${formatNumber(result.messagesPerSecond)} round trips/s ` +
                            `(${formatNumber((performance.now() - start) / 1000, 1)} s)`
                        );
                    }
                }
            }
        }
    }

    console.log('\nPerformance medians');
    console.table(aggregatePerformance(
        performanceResults,
        options.implementations,
        options.payloads,
        options.messageTypes,
        options.compression
    ));

    console.log('\nIdle-connection memory benchmark');
    for (const compression of options.compression) {
        for (const implementation of options.implementations) {
            process.stdout.write(
                `  ${implementation}, compression ${compression} ... `
            );
            const result = await runMemoryCase(
                implementation,
                compression,
                options
            );
            memoryResults.push(result);
            console.log(
                `${formatNumber(result.bytesPerConnection / 1024, 2)} KiB/connection`
            );
        }
    }
    console.log('\nMemory results');
    console.table(memoryTable(memoryResults));

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
                utf8Validate: require('utf-8-validate/package.json').version,
                serverBufferutil: true,
                loadGeneratorBufferutil: false,
                utf8Path: typeof isUtf8 === 'function'
                    ? 'node:buffer.isUtf8'
                    : 'utf-8-validate'
            },
            options
        },
        performance: performanceResults,
        memory: memoryResults
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
