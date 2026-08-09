'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const zlib = require('node:zlib');
const { fork } = require('node:child_process');
const { once } = require('node:events');

const WebSocket = require('ws');
const bufferutil = require('bufferutil');

const benchmarkDirectory = __dirname;
const repositoryDirectory = path.join(benchmarkDirectory, '..');
const workerPath = path.join(benchmarkDirectory, 'ingress-server-worker.js');
const OPEN_BATCH_SIZE = 100;

function parsePositiveNumber(value, option) {
    const number = Number(value);
    if (!Number.isFinite(number) || number <= 0) {
        throw new TypeError(`${option} must be a positive number`);
    }
    return number;
}

function parsePositiveInteger(value, option) {
    const number = parsePositiveNumber(value, option);
    if (!Number.isInteger(number)) throw new TypeError(`${option} must be an integer`);
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
        dataModes: ['text', 'binary', 'app-deflate'],
        sourceContent: 'ascii',
        serverTextOutput: 'buffer',
        textConsumption: 'native',
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
        } else if (option === '--data-modes') {
            options.dataModes = value.split(',').filter(Boolean);
        } else if (option === '--source-content') {
            options.sourceContent = value;
        } else if (option === '--server-text-output') {
            options.serverTextOutput = value;
        } else if (option === '--text-consumption') {
            options.textConsumption = value;
        } else if (option === '--output') {
            options.output = path.resolve(value);
        } else {
            throw new TypeError(`unknown option: ${option}`);
        }
    }
    const implementations = new Set(['current', 'eiows-9.2.0', 'ws']);
    if (!options.implementations.length ||
        options.implementations.some((value) => !implementations.has(value))) {
        throw new TypeError('--implementations supports current, eiows-9.2.0 and ws');
    }
    if (!options.dataModes.length ||
        options.dataModes.some((value) =>
            !['text', 'binary', 'app-deflate'].includes(value))) {
        throw new TypeError('--data-modes supports text, binary and app-deflate');
    }
    if (!['native', 'string'].includes(options.textConsumption)) {
        throw new TypeError('--text-consumption supports native and string');
    }
    if (!['ascii', 'mixed-utf8'].includes(options.sourceContent)) {
        throw new TypeError('--source-content supports ascii and mixed-utf8');
    }
    if (!['buffer', 'string'].includes(options.serverTextOutput)) {
        throw new TypeError('--server-text-output supports buffer and string');
    }
    return options;
}

function modulePathFor(implementation) {
    if (implementation === 'current') return repositoryDirectory;
    return path.join(benchmarkDirectory, 'node_modules', implementation);
}

function createSource(size, sourceContent) {
    const chunks = [];
    let bytes = 0;
    let sequence = 0;
    while (bytes < size) {
        const record = {
            event: 'update',
            room: `room-${sequence % 64}`,
            sequence,
            value: Math.imul(sequence, 2654435761) >>> 0,
            active: (sequence & 1) === 0
        };
        if (sourceContent === 'mixed-utf8') record.label = 'café-東京-🙂';
        const chunk = Buffer.from(`${JSON.stringify(record)}\n`);
        chunks.push(chunk);
        bytes += chunk.length;
        sequence++;
    }
    return Buffer.concat(chunks, bytes).subarray(0, size);
}

function createPayload(sourceSize, dataMode, sourceContent) {
    const source = createSource(sourceSize, sourceContent);
    if (dataMode === 'text') {
        return { payload: source.toString('utf8'), binary: false };
    }
    if (dataMode === 'app-deflate') {
        return { payload: zlib.deflateRawSync(source), binary: true };
    }
    return { payload: source, binary: true };
}

class IngressServer {
    constructor(implementation, binary, payloadBytes, textConsumption, serverTextOutput) {
        this.implementation = implementation;
        this.nextRequestId = 1;
        this.pending = new Map();
        this.ready = new Promise((resolve, reject) => {
            this.resolveReady = resolve;
            this.rejectReady = reject;
        });
        const environment = { ...process.env };
        delete environment.WS_NO_BUFFER_UTIL;
        delete environment.WS_NO_UTF_8_VALIDATE;
        const execArgv = ['--expose-gc'];
        const cpuProfileDirectory = process.env.EIOWS_BENCHMARK_CPU_PROF_DIR;
        if (cpuProfileDirectory) {
            fs.mkdirSync(cpuProfileDirectory, { recursive: true });
            execArgv.push('--cpu-prof', `--cpu-prof-dir=${cpuProfileDirectory}`);
        }
        this.child = fork(
            workerPath,
            [
                implementation,
                modulePathFor(implementation),
                String(binary),
                String(payloadBytes),
                textConsumption,
                serverTextOutput
            ],
            {
                stdio: ['ignore', 'inherit', 'inherit', 'ipc'],
                execArgv,
                env: environment
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

    async shutdown() {
        if (!this.child.connected) return;
        await this.request('shutdown');
        if (this.child.exitCode === null) await once(this.child, 'exit');
    }

    terminate() {
        if (this.child.exitCode === null) this.child.kill('SIGTERM');
    }
}

function openClient(url) {
    return new Promise((resolve, reject) => {
        const client = new WebSocket(url, { perMessageDeflate: false });
        const onError = (error) => {
            client.removeListener('open', onOpen);
            reject(error);
        };
        const onOpen = () => {
            client.removeListener('error', onError);
            client.on('error', () => {});
            resolve(client);
        };
        client.once('error', onError);
        client.once('open', onOpen);
    });
}

async function addClients(clients, url, count) {
    let remaining = count;
    while (remaining > 0) {
        const batchSize = Math.min(OPEN_BATCH_SIZE, remaining);
        clients.push(...await Promise.all(
            Array.from({ length: batchSize }, () => openClient(url))
        ));
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

function runClientLoad(clients, payload, binary, inflight, durationSeconds) {
    const yieldStride = 256;
    let active = true;
    let sent = 0;
    let completed = 0;
    let outstanding = 0;
    let settled = false;
    return new Promise((resolve, reject) => {
        const finish = () => {
            if (!active && outstanding === 0 && !settled) {
                settled = true;
                resolve(sent);
            }
        };
        const sendOne = (client) => {
            if (!active) return;
            sent++;
            outstanding++;
            client.send(payload, { binary, compress: false }, (error) => {
                outstanding--;
                completed++;
                if (error && !settled) {
                    settled = true;
                    reject(error);
                    return;
                }
                if (active && completed % yieldStride === 0) {
                    setImmediate(() => {
                        if (active) sendOne(client);
                        else finish();
                    });
                } else if (active) {
                    sendOne(client);
                }
                finish();
            });
        };
        for (const client of clients) {
            for (let index = 0; index < inflight; index++) sendOne(client);
        }
        setTimeout(() => {
            active = false;
            finish();
        }, durationSeconds * 1000);
    });
}

async function waitForCount(server, expectedMessages, timeoutMs = 15000) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
        const snapshot = await server.request('snapshot');
        if (snapshot.invalidMessage) throw new Error(snapshot.invalidMessage);
        if (snapshot.messages >= expectedMessages) return snapshot;
        if (Date.now() >= deadline) {
            throw new Error(
                `server received ${snapshot.messages} of ${expectedMessages} messages`
            );
        }
        await new Promise((resolve) => setTimeout(resolve, 10));
    }
}

async function runCase(implementation, sourceSize, dataMode, options) {
    const { payload, binary } = createPayload(
        sourceSize,
        dataMode,
        options.sourceContent
    );
    const payloadBytes = Buffer.byteLength(payload);
    const server = new IngressServer(
        implementation,
        binary,
        payloadBytes,
        options.textConsumption,
        options.serverTextOutput
    );
    const clients = [];
    try {
        const ready = await server.ready;
        await addClients(clients, `ws://127.0.0.1:${ready.port}`, options.connections);
        await server.request('reset');
        const warmupMessages = await runClientLoad(
            clients,
            payload,
            binary,
            options.inflight,
            options.warmup
        );
        await waitForCount(server, warmupMessages);

        await server.request('begin-measurement');
        const sent = await runClientLoad(
            clients,
            payload,
            binary,
            options.inflight,
            options.duration
        );
        await waitForCount(server, sent);
        const result = await server.request('finish-measurement', { expectedMessages: sent });
        if (result.invalidMessage) throw new Error(result.invalidMessage);
        if (result.messages !== sent) {
            throw new Error(`expected ${sent} messages, received ${result.messages}`);
        }
        return {
            implementation,
            sourceSize,
            dataMode,
            wirePayloadBytes: payloadBytes,
            compressionRatio: payloadBytes / sourceSize,
            connections: options.connections,
            ...result,
            logicalMiBPerSecond:
                result.messagesPerSecond * sourceSize / 1024 / 1024,
            wireMiBPerSecond:
                result.messagesPerSecond * payloadBytes / 1024 / 1024
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

function format(value, digits = 0) {
    return value.toLocaleString('en-US', {
        minimumFractionDigits: digits,
        maximumFractionDigits: digits
    });
}

function aggregate(results, options) {
    const rows = [];
    for (const sourceSize of options.payloads) {
        for (const dataMode of options.dataModes) {
            for (const implementation of options.implementations) {
                const matching = results.filter((result) =>
                    result.sourceSize === sourceSize &&
                    result.dataMode === dataMode &&
                    result.implementation === implementation);
                const value = (property) => median(
                    matching.map((result) => result[property])
                );
                rows.push({
                    implementation,
                    source: `${format(sourceSize)} B`,
                    data: dataMode,
                    'wire payload B': format(value('wirePayloadBytes')),
                    'messages/s': format(value('messagesPerSecond')),
                    'logical MiB/s': format(value('logicalMiBPerSecond'), 1),
                    'wire MiB/s': format(value('wireMiBPerSecond'), 1),
                    'server CPU %': format(value('serverCpuPercent'), 1),
                    'M messages/CPU-s': format(
                        value('millionMessagesPerCpuSecond'),
                        3
                    ),
                    'peak RSS +MiB': format(
                        value('activePeakRssDeltaBytes') / 1024 / 1024,
                        1
                    )
                });
            }
        }
    }
    return rows;
}

async function main() {
    const options = parseArguments(process.argv.slice(2));
    if (typeof bufferutil.mask !== 'function') {
        throw new Error('bufferutil did not load in the ingress generator');
    }
    for (const dependency of ['eiows-9.2.0', 'ws', 'bufferutil']) {
        if (!fs.existsSync(path.join(benchmarkDirectory, 'node_modules', dependency))) {
            throw new Error(`missing ${dependency}; run "npm install --prefix benchmark" first`);
        }
    }

    const startedAt = new Date().toISOString();
    const results = [];
    console.log('Ingress benchmark: permessage-deflate off; client bufferutil enabled');
    console.log('Configuration:', JSON.stringify(options));
    for (const sourceSize of options.payloads) {
        for (const dataMode of options.dataModes) {
            for (let iteration = 0; iteration < options.iterations; iteration++) {
                const rotated = options.implementations.map((_, index) =>
                    options.implementations[
                        (index + iteration) % options.implementations.length
                    ]);
                for (const implementation of rotated) {
                    process.stdout.write(
                        `  ${implementation}, ${sourceSize} B, ${dataMode}, ` +
                        `iteration ${iteration + 1}/${options.iterations} ... `
                    );
                    const result = await runCase(
                        implementation,
                        sourceSize,
                        dataMode,
                        options
                    );
                    results.push(result);
                    console.log(
                        `${format(result.messagesPerSecond)} messages/s, ` +
                        `${format(result.serverCpuPercent, 1)}% CPU`
                    );
                }
            }
        }
    }

    console.log('\nIngress medians');
    console.table(aggregate(results, options));
    const report = {
        metadata: {
            startedAt,
            finishedAt: new Date().toISOString(),
            node: process.version,
            platform: process.platform,
            architecture: process.arch,
            cpus: os.cpus().map((cpu) => cpu.model),
            totalMemoryBytes: os.totalmem(),
            wsClientNativeSupport: { bufferutil: require('bufferutil/package.json').version },
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
