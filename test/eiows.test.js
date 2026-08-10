'use strict';

const assert = require('node:assert/strict');
const asyncHooks = require('node:async_hooks');
const { EventEmitter } = require('node:events');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const net = require('node:net');
const path = require('node:path');
const tls = require('node:tls');
const test = require('node:test');
const { Worker } = require('node:worker_threads');
const zlib = require('node:zlib');
const { Server: EngineIo } = require('engine.io');

const eiows = require('..');
const nativeBinary = `../dist/eiows_${process.versions.modules}.node`;
const native = require(nativeBinary);

const fixtures = path.join(__dirname, 'fixtures');
const tlsOptions = {
    key: fs.readFileSync(path.join(fixtures, 'key.pem')),
    cert: fs.readFileSync(path.join(fixtures, 'cert.pem'))
};

function clientFrame(data, options = {}) {
    const payload = Buffer.isBuffer(data) ? data : Buffer.from(data);
    const mask = Buffer.from([0x12, 0x34, 0x56, 0x78]);
    const headerLength = payload.length < 126 ? 2 : payload.length <= 0xffff ? 4 : 10;
    const frame = Buffer.alloc(headerLength + 4 + payload.length);
    frame[0] = (options.fin === false ? 0 : 0x80) |
        (options.compressed ? 0x40 : 0) |
        (options.opCode === undefined ? 1 : options.opCode);
    if (payload.length < 126) {
        frame[1] = 0x80 | payload.length;
    } else if (payload.length <= 0xffff) {
        frame[1] = 0x80 | 126;
        frame.writeUInt16BE(payload.length, 2);
    } else {
        frame[1] = 0x80 | 127;
        frame.writeBigUInt64BE(BigInt(payload.length), 2);
    }
    mask.copy(frame, headerLength);
    for (let index = 0; index < payload.length; index++) {
        frame[headerLength + 4 + index] = payload[index] ^ mask[index & 3];
    }
    return frame;
}

function parseServerFrame(buffer) {
    if (buffer.length < 2) return null;
    const opCode = buffer[0] & 0x0f;
    const compressed = Boolean(buffer[0] & 0x40);
    let payloadLength = buffer[1] & 0x7f;
    let offset = 2;
    if (payloadLength === 126) {
        if (buffer.length < 4) return null;
        payloadLength = buffer.readUInt16BE(2);
        offset = 4;
    } else if (payloadLength === 127) {
        if (buffer.length < 10) return null;
        payloadLength = Number(buffer.readBigUInt64BE(2));
        offset = 10;
    }
    if (buffer.length < offset + payloadLength) return null;
    return {
        opCode,
        compressed,
        payload: buffer.subarray(offset, offset + payloadLength),
        consumed: offset + payloadLength
    };
}

function listen(server) {
    return new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(0, '127.0.0.1', () => {
            server.removeListener('error', reject);
            resolve(server.address().port);
        });
    });
}

function closeServer(server) {
    return new Promise((resolve) => server.close(resolve));
}

function destroySocket(socket) {
    return new Promise((resolve) => {
        socket.once('close', resolve);
        socket.destroy();
    });
}

function collectUntilSocketClose(socket, timeoutMs = 3000) {
    return new Promise((resolve, reject) => {
        const chunks = [];
        const timeout = setTimeout(
            () => finish(new Error('timed out waiting for socket close')),
            timeoutMs
        );
        const cleanup = () => {
            clearTimeout(timeout);
            socket.removeListener('data', onData);
            socket.removeListener('error', onError);
            socket.removeListener('close', onClose);
        };
        const finish = (error) => {
            cleanup();
            if (error) reject(error);
            else resolve(Buffer.concat(chunks));
        };
        const onData = (chunk) => chunks.push(chunk);
        const onError = (error) => finish(error);
        const onClose = () => finish();
        socket.on('data', onData);
        socket.once('error', onError);
        socket.once('close', onClose);
    });
}

function connect(port, secure) {
    return secure
        ? tls.connect({ port, host: '127.0.0.1', rejectUnauthorized: false })
        : net.connect({ port, host: '127.0.0.1' });
}

function websocketRequest(extraHeaders = '') {
    return 'GET /engine.io/?EIO=4&transport=websocket HTTP/1.1\r\n' +
        'Host: localhost\r\n' +
        'Upgrade: websocket\r\n' +
        'Connection: keep-alive, Upgrade\r\n' +
        'Sec-WebSocket-Version: 13\r\n' +
        'Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n' +
        extraHeaders +
        '\r\n';
}

function createServerFrameReader(socket) {
    let buffer = Buffer.alloc(0);
    let upgraded = false;
    const frames = [];
    const waiters = [];

    const flush = () => {
        if (!upgraded) {
            const headerEnd = buffer.indexOf('\r\n\r\n');
            if (headerEnd === -1) return;
            buffer = buffer.subarray(headerEnd + 4);
            upgraded = true;
        }
        for (;;) {
            const frame = parseServerFrame(buffer);
            if (!frame) break;
            buffer = buffer.subarray(frame.consumed);
            const waiter = waiters.shift();
            if (waiter) waiter.resolve(frame);
            else frames.push(frame);
        }
    };

    socket.on('data', (chunk) => {
        buffer = Buffer.concat([buffer, chunk]);
        flush();
    });

    return function nextFrame(timeoutMs = 3000) {
        if (frames.length) return Promise.resolve(frames.shift());
        return new Promise((resolve, reject) => {
            const waiter = {
                resolve: (frame) => {
                    clearTimeout(timeout);
                    resolve(frame);
                }
            };
            const timeout = setTimeout(() => {
                const index = waiters.indexOf(waiter);
                if (index !== -1) waiters.splice(index, 1);
                reject(new Error('timed out waiting for WebSocket frame'));
            }, timeoutMs);
            waiters.push(waiter);
            flush();
        });
    };
}

class CapturingSocket extends EventEmitter {
    constructor() {
        super();
        this.destroyed = false;
        this.writable = true;
        this.writableLength = 0;
        this.writes = [];
        this.corkCount = 0;
    }

    write(data, callback) {
        this.writes.push(data);
        if (callback) process.nextTick(callback);
        return true;
    }

    cork() {
        this.corkCount++;
    }

    uncork() {}

    destroy() {
        if (this.destroyed) return;
        this.destroyed = true;
        this.writable = false;
        process.nextTick(() => this.emit('close'));
    }

    end() {
        this.destroy();
    }
}

class RejectedUpgradeSocket extends EventEmitter {
    constructor() {
        super();
        this.destroyed = false;
        this.writable = true;
        this.response = '';
    }

    write() {
        return true;
    }

    end(response) {
        this.response = response;
        this.writable = false;
        process.nextTick(() => this.emit('finish'));
    }

    destroy() {
        this.destroyed = true;
    }
}

async function runEchoCase(secure, textAsString = false) {
    const server = secure ? https.createServer(tlsOptions) : http.createServer();
    const wsServer = new eiows.Server({ maxPayload: 1024, textAsString });
    const messages = [];
    wsServer.on('headers', (headers) => headers.push('X-Eiows-Test: yes'));
    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (webSocket, callbackRequest) => {
            assert.equal(callbackRequest, request);
            assert.equal(webSocket._nativeTransport, true);
            assert.equal(webSocket._upgradePending, false);
            assert.equal(webSocket.readyState, eiows.OPEN);
            assert.equal(webSocket._transportSocket, null);
            assert.notEqual(webSocket._socket, socket);
            assert.equal(socket.destroyed, true);
            assert.equal(socket._handle, null);
            webSocket.on('error', () => {});
            webSocket.on('message', (message, isBinary) => {
                messages.push([message, isBinary]);
                if (secure) {
                    webSocket.send(message.toString());
                } else {
                    const payload = Buffer.from(message);
                    webSocket._sender.sendFrame([
                        Buffer.from([0x81, payload.length]),
                        payload
                    ], (error) => assert.ifError(error));
                }
            });
        });
    });

    const port = await listen(server);
    const socket = connect(port, secure);
    const received = [];
    socket.on('data', (chunk) => received.push(chunk));
    await new Promise((resolve, reject) => {
        socket.once(secure ? 'secureConnect' : 'connect', resolve);
        socket.once('error', reject);
    });

    // Coalescing the first WebSocket frame with the HTTP request exercises upgradeHead.
    socket.write(Buffer.concat([
        Buffer.from(websocketRequest()),
        clientFrame('hello')
    ]));

    const result = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error('timed out waiting for echo')), 3000);
        const inspect = () => {
            const data = Buffer.concat(received);
            const headerEnd = data.indexOf('\r\n\r\n');
            if (headerEnd === -1) return;
            const frame = parseServerFrame(data.subarray(headerEnd + 4));
            if (!frame) return;
            clearTimeout(timeout);
            resolve({ headers: data.subarray(0, headerEnd).toString(), frame });
        };
        socket.on('data', inspect);
        inspect();
    });

    assert.match(result.headers, /^HTTP\/1\.1 101 Switching Protocols/m);
    assert.match(result.headers, /^X-Eiows-Test: yes$/m);
    assert.equal(result.frame.opCode, 1);
    assert.equal(result.frame.payload.toString(), 'hello');
    assert.deepEqual(messages, [[textAsString ? 'hello' : Buffer.from('hello'), false]]);

    await destroySocket(socket);
    await new Promise((resolve) => wsServer.close(resolve));
    await closeServer(server);
}

test('takes ownership from Node TCP and consumes upgradeHead natively', () => runEchoCase(false));
test('takes ownership of the descriptor and SSL state from TLSWrap', () => runEchoCase(true));
test('can restore legacy string messages with textAsString', () => runEchoCase(false, true));

async function runCloseDuringPendingUpgradeCase(secure) {
    const server = secure ? https.createServer(tlsOptions) : http.createServer();
    const wsServer = new eiows.Server({ perMessageDeflate: false });
    let callbackCalled = false;
    let transportClosed = false;
    let resolveServerClosed;
    const serverClosed = new Promise((resolve) => { resolveServerClosed = resolve; });

    server.on('upgrade', (request, transport, head) => {
        transport.once('close', () => { transportClosed = true; });
        wsServer.handleUpgrade(request, transport, head, () => {
            callbackCalled = true;
        });
        // Run before the setImmediate() that transfers the descriptor. This is
        // the production shutdown race that previously queued a close frame
        // ahead of the HTTP 101 response.
        process.nextTick(() => wsServer.close(resolveServerClosed));
    });

    const port = await listen(server);
    const socket = connect(port, secure);
    try {
        await new Promise((resolve, reject) => {
            socket.once(secure ? 'secureConnect' : 'connect', resolve);
            socket.once('error', reject);
        });
        const responsePromise = collectUntilSocketClose(socket);
        socket.write(websocketRequest());
        const response = await responsePromise;
        await serverClosed;

        assert.match(response.toString(), /^HTTP\/1\.1 503 Service Unavailable\r\n/);
        assert.equal(response[0], 0x48, 'HTTP must be the first byte on the wire');
        assert.equal(callbackCalled, false);
        assert.equal(transportClosed, true);
        assert.equal(wsServer._clients.size, 0);
        assert.equal(wsServer._closed, true);
    } finally {
        if (!socket.destroyed) await destroySocket(socket);
        if (!wsServer._closed) await new Promise((resolve) => wsServer.close(resolve));
        if (server.listening) await closeServer(server);
    }
}

async function runCloseFromHeadersCase(secure) {
    const server = secure ? https.createServer(tlsOptions) : http.createServer();
    const wsServer = new eiows.Server({ perMessageDeflate: false });
    let callbackCalled = false;
    let resolveServerClosed;
    const serverClosed = new Promise((resolve) => { resolveServerClosed = resolve; });
    wsServer.once('headers', () => wsServer.close(resolveServerClosed));
    server.on('upgrade', (request, transport, head) => {
        wsServer.handleUpgrade(request, transport, head, () => {
            callbackCalled = true;
        });
    });

    const port = await listen(server);
    const socket = connect(port, secure);
    try {
        await new Promise((resolve, reject) => {
            socket.once(secure ? 'secureConnect' : 'connect', resolve);
            socket.once('error', reject);
        });
        const responsePromise = collectUntilSocketClose(socket);
        socket.write(websocketRequest());
        const response = await responsePromise;
        await serverClosed;

        assert.match(response.toString(), /^HTTP\/1\.1 503 Service Unavailable\r\n/);
        assert.equal(callbackCalled, false);
        assert.equal(wsServer._clients.size, 0);
        assert.equal(wsServer._closed, true);
    } finally {
        if (!socket.destroyed) await destroySocket(socket);
        if (!wsServer._closed) await new Promise((resolve) => wsServer.close(resolve));
        if (server.listening) await closeServer(server);
    }
}

for (const secure of [false, true]) {
    const transport = secure ? 'TLS' : 'TCP';
    test(`Server.close aborts a pending native ${transport} upgrade before framing`, () =>
        runCloseDuringPendingUpgradeCase(secure));
    test(`Server.close from headers rejects the ${transport} upgrade`, () =>
        runCloseFromHeadersCase(secure));
}

test('dispatches owned transport events through their AsyncResource', async () => {
    const server = http.createServer();
    const wsServer = new eiows.Server({});
    const initialized = [];
    let ownedAsyncId = null;
    let resolveDestroyed;
    const resourceDestroyed = new Promise((resolve) => { resolveDestroyed = resolve; });
    let observed = null;
    let resolveMessage;
    const message = new Promise((resolve) => { resolveMessage = resolve; });
    const hook = asyncHooks.createHook({
        init(asyncId, type, triggerAsyncId, resource) {
            if (type === 'eiows.ownedTransport') {
                ownedAsyncId = asyncId;
                initialized.push({ asyncId, triggerAsyncId, resource });
            }
        },
        destroy(asyncId) {
            if (asyncId === ownedAsyncId) resolveDestroyed();
        }
    });
    hook.enable();

    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (webSocket) => {
            webSocket.on('error', () => {});
            webSocket.on('message', () => {
                observed = {
                    asyncId: asyncHooks.executionAsyncId(),
                    resource: asyncHooks.executionAsyncResource()
                };
                resolveMessage();
            });
        });
    });

    const port = await listen(server);
    const socket = net.connect({ port, host: '127.0.0.1' });
    try {
        await new Promise((resolve, reject) => {
            socket.once('connect', resolve);
            socket.once('error', reject);
        });
        socket.write(Buffer.concat([
            Buffer.from(websocketRequest()),
            clientFrame('async-context')
        ]));
        await message;

        assert.equal(initialized.length, 1);
        assert.equal(observed.asyncId, initialized[0].asyncId);
        assert.equal(observed.resource, initialized[0].resource);
        await destroySocket(socket);
        await closeServer(server);
        await new Promise((resolve, reject) => {
            const timeout = setTimeout(
                () => reject(new Error('timed out waiting for AsyncResource destroy')),
                1000
            );
            resourceDestroyed.then(() => {
                clearTimeout(timeout);
                resolve();
            }, reject);
        });
    } finally {
        hook.disable();
        if (!socket.destroyed) await destroySocket(socket);
        if (server.listening) await closeServer(server);
    }
});

test('owns an SNI TLS context without disturbing regular Node TLS connections', async () => {
    const alternateContext = tls.createSecureContext(tlsOptions);
    const server = https.createServer({
        ...tlsOptions,
        SNICallback(servername, callback) {
            callback(null, servername === 'owned.local' ? alternateContext : null);
        }
    }, (request, response) => {
        response.end('node-tls-ok');
    });
    const wsServer = new eiows.Server({ perMessageDeflate: false });
    let ownedWebSocket;
    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (webSocket) => {
            ownedWebSocket = webSocket;
            webSocket.on('error', () => {});
            webSocket.on('message', (message) => webSocket.send(message));
        });
    });

    const port = await listen(server);
    const ownedSocket = tls.connect({
        port,
        host: '127.0.0.1',
        servername: 'owned.local',
        rejectUnauthorized: false
    });
    const nextFrame = createServerFrameReader(ownedSocket);
    await new Promise((resolve, reject) => {
        ownedSocket.once('secureConnect', resolve);
        ownedSocket.once('error', reject);
    });
    ownedSocket.write(Buffer.concat([
        Buffer.from(websocketRequest()),
        clientFrame('owned-sni')
    ]));
    const echoed = await nextFrame();
    assert.equal(echoed.payload.toString(), 'owned-sni');
    assert.equal(ownedWebSocket._nativeTransport, true);
    assert.equal(ownedWebSocket._transportSocket, null);

    const regularBody = await new Promise((resolve, reject) => {
        https.get({
            port,
            host: '127.0.0.1',
            servername: 'node.local',
            rejectUnauthorized: false
        }, (response) => {
            const chunks = [];
            response.on('data', (chunk) => chunks.push(chunk));
            response.once('end', () => resolve(Buffer.concat(chunks).toString()));
        }).once('error', reject);
    });
    assert.equal(regularBody, 'node-tls-ok');

    await destroySocket(ownedSocket);
    await new Promise((resolve) => wsServer.close(resolve));
    await closeServer(server);
});

async function runVectoredFrameCase(secure) {
    const server = secure ? https.createServer(tlsOptions) : http.createServer();
    const wsServer = new eiows.Server({ maxPayload: 1024 });
    let resolveWrite;
    let rejectWrite;
    const writeFinished = new Promise((resolve, reject) => {
        resolveWrite = resolve;
        rejectWrite = reject;
    });

    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (webSocket) => {
            assert.equal(webSocket._nativeTransport, true);
            webSocket.on('error', rejectWrite);
            webSocket._sender.sendFrame([
                Buffer.from([0x82, 0x0b]),
                Buffer.from('vector'),
                new DataView(Uint8Array.from([0x2d, 0x70]).buffer),
                'ath'
            ], (error) => error ? rejectWrite(error) : resolveWrite());
        });
    });

    const port = await listen(server);
    const socket = connect(port, secure);
    const nextFrame = createServerFrameReader(socket);
    await new Promise((resolve, reject) => {
        socket.once(secure ? 'secureConnect' : 'connect', resolve);
        socket.once('error', reject);
    });
    socket.write(websocketRequest());

    const frame = await nextFrame();
    assert.equal(frame.opCode, 2);
    assert.equal(frame.payload.toString(), 'vector-path');
    await writeFinished;

    await destroySocket(socket);
    await new Promise((resolve) => wsServer.close(resolve));
    await closeServer(server);
}

test('writes multi-part native TCP vectors in one request', () => runVectoredFrameCase(false));
test('serializes multi-part native vectors through owned TLS', () => runVectoredFrameCase(true));

test('batches frame lists larger than the platform I/O-vector limit', async () => {
    const server = http.createServer();
    const wsServer = new eiows.Server({ maxPayload: 4096 });
    const payloadLength = 2048;
    let resolveWrite;
    let rejectWrite;
    const writeFinished = new Promise((resolve, reject) => {
        resolveWrite = resolve;
        rejectWrite = reject;
    });
    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (webSocket) => {
            webSocket.on('error', rejectWrite);
            const header = Buffer.alloc(4);
            header[0] = 0x82;
            header[1] = 126;
            header.writeUInt16BE(payloadLength, 2);
            const parts = [header];
            for (let index = 0; index < payloadLength; index++) {
                parts.push(Buffer.from([index & 0xff]));
            }
            webSocket._sender.sendFrame(
                parts,
                (error) => error ? rejectWrite(error) : resolveWrite()
            );
        });
    });

    const port = await listen(server);
    const socket = connect(port, false);
    const nextFrame = createServerFrameReader(socket);
    await new Promise((resolve, reject) => {
        socket.once('connect', resolve);
        socket.once('error', reject);
    });
    socket.write(websocketRequest());
    const frame = await nextFrame();
    assert.equal(frame.payload.length, payloadLength);
    for (let index = 0; index < payloadLength; index++) {
        assert.equal(frame.payload[index], index & 0xff);
    }
    await writeFinished;

    await destroySocket(socket);
    await new Promise((resolve) => wsServer.close(resolve));
    await closeServer(server);
});

async function runPeerCloseCase(secure) {
    const server = secure ? https.createServer(tlsOptions) : http.createServer();
    const wsServer = new eiows.Server({ maxPayload: 1024 });
    let resolveCloseEvent;
    const closeEventPromise = new Promise((resolve) => {
        resolveCloseEvent = resolve;
    });
    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (webSocket) => {
            assert.equal(webSocket._nativeTransport, true);
            webSocket.on('error', () => {});
            webSocket.once('close', (code, reason) => {
                resolveCloseEvent([code, reason]);
            });
        });
    });

    const port = await listen(server);
    const socket = connect(port, secure);
    const nextFrame = createServerFrameReader(socket);
    await new Promise((resolve, reject) => {
        socket.once(secure ? 'secureConnect' : 'connect', resolve);
        socket.once('error', reject);
    });

    const closePayload = Buffer.alloc(5);
    closePayload.writeUInt16BE(1000, 0);
    closePayload.write('bye', 2);
    socket.write(Buffer.concat([
        Buffer.from(websocketRequest()),
        clientFrame(closePayload, { opCode: 8 })
    ]));

    const echoedClose = await nextFrame();
    assert.equal(echoedClose.opCode, 8);
    assert.equal(echoedClose.payload.readUInt16BE(0), 1000);
    assert.equal(echoedClose.payload.subarray(2).toString(), 'bye');
    await new Promise((resolve) => socket.once('close', resolve));
    assert.deepEqual(await closeEventPromise, [1000, 'bye']);

    await new Promise((resolve) => wsServer.close(resolve));
    await closeServer(server);
}

test('flushes a peer close frame before ending native TCP', () => runPeerCloseCase(false));
test('flushes a peer close frame before ending native TLS', () => runPeerCloseCase(true));

test('serializes native TLS writes and reports only remaining queued bytes', async () => {
    const server = https.createServer(tlsOptions);
    const wsServer = new eiows.Server({ maxPayload: 1024 * 1024 });
    const writeCount = 32;
    const callbackOrder = [];
    let queuedAmount = 0;
    let resolveCallbacks;
    const callbacksDone = new Promise((resolve) => {
        resolveCallbacks = resolve;
    });

    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (webSocket) => {
            assert.equal(webSocket._nativeTransport, true);
            webSocket.on('error', () => {});
            for (let index = 0; index < writeCount; index++) {
                const payload = Buffer.alloc(16 * 1024, index);
                webSocket.send(payload, (error) => {
                    assert.ifError(error);
                    callbackOrder.push(index);
                    if (callbackOrder.length === writeCount) resolveCallbacks();
                });
            }
            queuedAmount = webSocket.bufferedAmount;
        });
    });

    const port = await listen(server);
    const socket = connect(port, true);
    const nextFrame = createServerFrameReader(socket);
    await new Promise((resolve, reject) => {
        socket.once('secureConnect', resolve);
        socket.once('error', reject);
    });
    socket.write(websocketRequest());

    for (let index = 0; index < writeCount; index++) {
        const frame = await nextFrame();
        assert.equal(frame.opCode, 2);
        assert.equal(frame.payload.length, 16 * 1024);
        assert.equal(frame.payload[0], index);
    }
    await callbacksDone;
    assert.ok(queuedAmount >= 0);
    assert.ok(queuedAmount <= writeCount * (16 * 1024 + 14));
    assert.deepEqual(callbackOrder, Array.from({ length: writeCount }, (_, index) => index));

    await destroySocket(socket);
    await new Promise((resolve) => wsServer.close(resolve));
    await closeServer(server);
});

test('retains TLS ciphertext and callbacks across raw socket backpressure', async () => {
    const server = https.createServer(tlsOptions);
    const wsServer = new eiows.Server({ maxPayload: 16 * 1024 * 1024 });
    const writeCount = 48;
    const payloadLength = 128 * 1024;
    let webSocket;
    let callbacks = 0;
    let resolveCallbacks;
    let rejectCallbacks;
    const callbacksDone = new Promise((resolve, reject) => {
        resolveCallbacks = resolve;
        rejectCallbacks = reject;
    });
    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (value) => {
            webSocket = value;
            webSocket.on('error', rejectCallbacks);
            for (let index = 0; index < writeCount; index++) {
                webSocket.send(Buffer.alloc(payloadLength, index), (error) => {
                    if (error) {
                        rejectCallbacks(error);
                        return;
                    }
                    callbacks++;
                    if (callbacks === writeCount) resolveCallbacks();
                });
            }
        });
    });

    const port = await listen(server);
    const socket = connect(port, true);
    const nextFrame = createServerFrameReader(socket);
    await new Promise((resolve, reject) => {
        socket.once('secureConnect', resolve);
        socket.once('error', reject);
    });
    socket.pause();
    socket.write(websocketRequest());
    await new Promise((resolve) => setTimeout(resolve, 50));
    assert.ok(webSocket, 'expected the TLS WebSocket upgrade to complete');
    assert.ok(callbacks < writeCount, 'backpressured writes completed prematurely');
    assert.ok(webSocket.bufferedAmount > 0, 'expected queued plaintext under backpressure');

    socket.resume();
    for (let index = 0; index < writeCount; index++) {
        const frame = await nextFrame(5000);
        assert.equal(frame.opCode, 2);
        assert.equal(frame.payload.length, payloadLength);
        assert.equal(frame.payload[0], index);
        assert.equal(frame.payload[payloadLength - 1], index);
    }
    await callbacksDone;
    assert.equal(webSocket.bufferedAmount, 0);

    await destroySocket(socket);
    await new Promise((resolve) => wsServer.close(resolve));
    await closeServer(server);
});

async function runDetachedArrayBufferWriteCase(secure) {
    const server = secure ? https.createServer(tlsOptions) : http.createServer();
    const payloadLength = 4 * 1024 * 1024;
    const wsServer = new eiows.Server({ maxPayload: payloadLength + 1024 });
    let resolveSubmitted;
    let rejectSubmitted;
    const submitted = new Promise((resolve, reject) => {
        resolveSubmitted = resolve;
        rejectSubmitted = reject;
    });
    let resolveWrite;
    const writeFinished = new Promise((resolve) => {
        resolveWrite = resolve;
    });

    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (webSocket) => {
            webSocket.on('error', rejectSubmitted);
            const payload = new ArrayBuffer(payloadLength);
            const bytes = new Uint8Array(payload);
            bytes[0] = 0x31;
            bytes[payloadLength - 1] = 0x7a;
            webSocket.send(payload, (error) => {
                if (error) rejectSubmitted(error);
                else resolveWrite();
            });
            structuredClone(payload, { transfer: [payload] });
            assert.equal(payload.byteLength, 0);
            resolveSubmitted();
        });
    });

    const port = await listen(server);
    const socket = connect(port, secure);
    const nextFrame = createServerFrameReader(socket);
    await new Promise((resolve, reject) => {
        socket.once(secure ? 'secureConnect' : 'connect', resolve);
        socket.once('error', reject);
    });
    socket.pause();
    socket.write(websocketRequest());
    await submitted;
    socket.resume();

    const frame = await nextFrame(5000);
    assert.equal(frame.opCode, 2);
    assert.equal(frame.payload.length, payloadLength);
    assert.equal(frame.payload[0], 0x31);
    assert.equal(frame.payload[payloadLength - 1], 0x7a);
    await writeFinished;

    await destroySocket(socket);
    await new Promise((resolve) => wsServer.close(resolve));
    await closeServer(server);
}

test('pins detached ArrayBuffer storage across native TCP backpressure', () =>
    runDetachedArrayBufferWriteCase(false));
test('pins detached ArrayBuffer storage across native TLS backpressure', () =>
    runDetachedArrayBufferWriteCase(true));

test('cleans up active owned TCP and TLS transports when a Worker exits', async () => {
    for (const secure of [false, true]) {
        const worker = new Worker(`
            'use strict';
            const http = require('node:http');
            const https = require('node:https');
            const { parentPort, workerData } = require('node:worker_threads');
            const eiows = require(workerData.modulePath);
            const server = workerData.secure
                ? https.createServer({ key: workerData.key, cert: workerData.cert })
                : http.createServer();
            const wsServer = new eiows.Server({ perMessageDeflate: false });
            server.on('upgrade', (request, socket, head) => {
                wsServer.handleUpgrade(request, socket, head, (webSocket) => {
                    webSocket.on('error', () => {});
                    parentPort.postMessage({ type: 'upgraded' });
                });
            });
            server.listen(0, '127.0.0.1', () => {
                parentPort.postMessage({ type: 'listening', port: server.address().port });
            });
        `, {
            eval: true,
            workerData: {
                modulePath: require.resolve('..'),
                secure,
                key: tlsOptions.key,
                cert: tlsOptions.cert
            }
        });
        const messages = [];
        const waiters = [];
        worker.on('message', (message) => {
            const waiter = waiters.shift();
            if (waiter) waiter.resolve(message);
            else messages.push(message);
        });
        const nextMessage = () => messages.length
            ? Promise.resolve(messages.shift())
            : new Promise((resolve, reject) => waiters.push({ resolve, reject }));

        const listening = await nextMessage();
        assert.equal(listening.type, 'listening');
        const socket = connect(listening.port, secure);
        await new Promise((resolve, reject) => {
            socket.once(secure ? 'secureConnect' : 'connect', resolve);
            socket.once('error', reject);
        });
        socket.write(websocketRequest());
        const upgraded = await nextMessage();
        assert.equal(upgraded.type, 'upgraded');

        let terminationTimer;
        const terminationTimeout = new Promise((_, reject) => {
            terminationTimer = setTimeout(
                () => reject(new Error('timed out terminating Worker with owned transport')),
                3000
            );
        });
        const exitCode = await Promise.race([worker.terminate(), terminationTimeout]);
        clearTimeout(terminationTimer);
        assert.equal(exitCode, 1);
        socket.destroy();
    }
});

test('Server defaults to ws-compatible Buffer text for Engine.IO', async () => {
    const wsServer = new eiows.Server({ perMessageDeflate: false });

    assert.equal(wsServer._textAsBuffer, true);
    await new Promise((resolve) => wsServer.close(resolve));
});

test('integrates with Engine.IO and round-trips Unicode text', async () => {
    const httpServer = http.createServer();
    const engine = new EngineIo({
        wsEngine: eiows.Server,
        transports: ['websocket'],
        perMessageDeflate: false,
        maxHttpBufferSize: 30 * 1024
    });
    engine.attach(httpServer);
    assert.ok(engine.ws instanceof eiows.Server);
    assert.equal(engine.ws._textAsBuffer, true);

    let receivedMessage;
    engine.once('connection', (engineSocket) => {
        engineSocket.once('message', (message) => {
            receivedMessage = message;
            engineSocket.send(`echo:${message}`);
        });
    });

    const port = await listen(httpServer);
    const socket = connect(port, false);
    const nextFrame = createServerFrameReader(socket);
    await new Promise((resolve, reject) => {
        socket.once('connect', resolve);
        socket.once('error', reject);
    });
    socket.write(websocketRequest());

    const openFrame = await nextFrame();
    assert.equal(openFrame.opCode, 1);
    assert.equal(openFrame.payload[0], 0x30, 'expected an Engine.IO open packet');

    const payload = 'café-東京-🙂';
    socket.write(clientFrame(`4${payload}`));
    const echoFrame = await nextFrame();
    assert.equal(echoFrame.opCode, 1);
    assert.equal(echoFrame.payload.toString(), `4echo:${payload}`);
    assert.equal(receivedMessage, payload);

    await destroySocket(socket);
    engine.close();
    await closeServer(httpServer);
});

test('exposes the same supported API through ESM and CommonJS', async () => {
    const esm = await import('../dist/wrapper.mjs');
    const exportedNames = [
        'WebSocket',
        'Server',
        'compressThreshold',
        'PERMESSAGE_DEFLATE',
        'SERVER_NO_CONTEXT_TAKEOVER',
        'CLIENT_NO_CONTEXT_TAKEOVER',
        'SLIDING_DEFLATE_WINDOW',
        'CONNECTING',
        'OPCODE_TEXT',
        'OPCODE_BINARY',
        'OPCODE_PING',
        'OPEN',
        'CLOSING',
        'CLOSED'
    ];

    assert.strictEqual(esm.default, eiows);
    for (const name of exportedNames) {
        assert.strictEqual(esm[name], eiows[name], `mismatched ESM export: ${name}`);
    }
    assert.equal(Object.hasOwn(eiows, 'native'), false);
    assert.equal(
        path.basename(require.resolve(nativeBinary)),
        `eiows_${process.versions.modules}.node`
    );
});

test('negotiates permessage-deflate and handles compressed client frames', async () => {
    const server = http.createServer();
    const wsServer = new eiows.Server({
        maxPayload: 1024 * 1024,
        perMessageDeflate: { threshold: 0 }
    });
    const payload = 'compressible payload '.repeat(100);
    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (webSocket) => {
            webSocket.on('error', () => {});
            webSocket.on('message', (message) => webSocket.send(message));
        });
    });

    const port = await listen(server);
    const socket = connect(port, false);
    await new Promise((resolve, reject) => {
        socket.once('connect', resolve);
        socket.once('error', reject);
    });

    const compressed = zlib.deflateRawSync(payload, {
        flush: zlib.constants.Z_SYNC_FLUSH,
        finishFlush: zlib.constants.Z_SYNC_FLUSH
    }).subarray(0, -4);
    socket.write(Buffer.concat([
        Buffer.from(websocketRequest('Sec-WebSocket-Extensions: permessage-deflate\r\n')),
        clientFrame(compressed, { compressed: true })
    ]));

    const chunks = [];
    const response = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error('timed out waiting for compressed echo')), 3000);
        socket.on('data', (chunk) => {
            chunks.push(chunk);
            const data = Buffer.concat(chunks);
            const headerEnd = data.indexOf('\r\n\r\n');
            if (headerEnd === -1) return;
            const frame = parseServerFrame(data.subarray(headerEnd + 4));
            if (!frame) return;
            clearTimeout(timeout);
            resolve({ headers: data.subarray(0, headerEnd).toString(), frame });
        });
    });

    assert.match(response.headers, /Sec-WebSocket-Extensions: permessage-deflate/);
    assert.equal(response.frame.compressed, true);
    const inflated = zlib.inflateRawSync(Buffer.concat([
        response.frame.payload,
        Buffer.from([0x00, 0x00, 0xff, 0xff])
    ]), { finishFlush: zlib.constants.Z_SYNC_FLUSH });
    assert.equal(inflated.toString(), payload);

    await destroySocket(socket);
    await new Promise((resolve) => wsServer.close(resolve));
    await closeServer(server);
});

test('only negotiates valid and supported permessage-deflate offers', () => {
    const options = eiows.PERMESSAGE_DEFLATE |
        eiows.CLIENT_NO_CONTEXT_TAKEOVER |
        eiows.SERVER_NO_CONTEXT_TAKEOVER;
    const context = native.createCompressionContext();
    const negotiate = (offer) => {
        const [session, response] = native.createSession(options, 1024, offer, context);
        native.dispose(session);
        return response;
    };

    assert.equal(negotiate('zzzzzzzzzzzzzzAA'), '');
    assert.equal(negotiate('9'.repeat(4096)), '');
    assert.equal(negotiate('permessage-deflate; server_max_window_bits=10'), '');
    assert.equal(
        negotiate('permessage-deflate; server_max_window_bits=10, permessage-deflate'),
        'permessage-deflate; client_no_context_takeover; server_no_context_takeover'
    );
    assert.equal(
        negotiate('permessage-deflate; client_no_context_takeover=invalid'),
        ''
    );
});

test('safely reuses no-context-takeover streams between sessions', () => {
    const options = eiows.PERMESSAGE_DEFLATE |
        eiows.CLIENT_NO_CONTEXT_TAKEOVER |
        eiows.SERVER_NO_CONTEXT_TAKEOVER;
    const context = native.createCompressionContext();
    const [first] = native.createSession(options, 4096, 'permessage-deflate', context);
    const [second] = native.createSession(options, 4096, 'permessage-deflate', context);
    const payload = 'shared compression context '.repeat(40);
    const compressed = zlib.deflateRawSync(payload, {
        flush: zlib.constants.Z_SYNC_FLUSH,
        finishFlush: zlib.constants.Z_SYNC_FLUSH
    }).subarray(0, -4);

    for (const session of [first, second]) {
        assert.deepEqual(
            native.consume(session, clientFrame(compressed, { compressed: true })),
            [[0, payload, false]]
        );
        const frame = parseServerFrame(native.frame(session, payload, 1, true));
        const inflated = zlib.inflateRawSync(Buffer.concat([
            frame.payload,
            Buffer.from([0x00, 0x00, 0xff, 0xff])
        ]), { finishFlush: zlib.constants.Z_SYNC_FLUSH });
        assert.equal(inflated.toString(), payload);
        native.dispose(session);
    }
});

test('rejects mismatched native handle types without corrupting either handle', () => {
    const options = eiows.PERMESSAGE_DEFLATE |
        eiows.CLIENT_NO_CONTEXT_TAKEOVER |
        eiows.SERVER_NO_CONTEXT_TAKEOVER;
    const context = native.createCompressionContext();
    const [session] = native.createSession(0, 1024, '');

    assert.throws(
        () => native.consume(context, Buffer.alloc(0)),
        { name: 'TypeError', message: 'expected a native WebSocket session' }
    );
    assert.throws(
        () => native.dispose(context),
        { name: 'TypeError', message: 'expected a native WebSocket session' }
    );
    assert.throws(
        () => native.createSession(options, 1024, 'permessage-deflate', session),
        { name: 'TypeError', message: 'expected a native compression context' }
    );

    const [compressedSession] = native.createSession(
        options,
        1024,
        'permessage-deflate',
        context
    );
    native.dispose(compressedSession);
    native.dispose(session);
});

test('destroys rejected upgrade sockets after flushing the response', async () => {
    const wsServer = new eiows.Server({});
    const socket = new RejectedUpgradeSocket();
    wsServer.handleUpgrade({ method: 'POST', headers: {} }, socket, Buffer.alloc(0), () => {});

    await new Promise((resolve) => setImmediate(resolve));
    assert.match(socket.response, /^HTTP\/1\.1 400 Bad Request/);
    assert.equal(socket.destroyed, true);
    await new Promise((resolve) => wsServer.close(resolve));
});

test('rejects unmasked client frames with a protocol close', () => {
    const [session] = native.createSession(0, 1024, '');
    const events = native.consume(session, Buffer.from([0x81, 0x04, 0x74, 0x65, 0x73, 0x74]));
    assert.equal(events[0][0], 1);
    const closeFrame = parseServerFrame(events[0][1]);
    assert.equal(closeFrame.opCode, 8);
    assert.equal(closeFrame.payload.readUInt16BE(0), 1002);
    assert.deepEqual(events[1], [2, 1006, '']);
    native.dispose(session);
});

test('parses a masked frame split at every possible byte boundary', () => {
    const expected = 'split-frame-payload';
    const completeFrame = clientFrame(expected);

    for (let split = 1; split < completeFrame.length; split++) {
        const [session] = native.createSession(0, 1024, '');
        const frame = Buffer.from(completeFrame);
        const firstEvents = native.consume(session, frame.subarray(0, split));
        const secondEvents = native.consume(session, frame.subarray(split));
        const events = firstEvents.concat(secondEvents);
        assert.deepEqual(events, [[0, expected, false]], `split at byte ${split}`);
        native.dispose(session);
    }
});

test('validates and unmasks large UTF-8 text on the SIMD path', () => {
    const payload = `${'a'.repeat(70)}€${'b'.repeat(70)}😀${'c'.repeat(70)}`;
    const [session] = native.createSession(0, Buffer.byteLength(payload), '');

    assert.deepEqual(
        native.consume(session, clientFrame(payload)),
        [[0, payload, false]]
    );
    native.dispose(session);
});

test('unmasks payloads correctly around SIMD boundaries', () => {
    const lengths = [1, 3, 4, 15, 16, 17, 63, 64, 65, 125, 126, 127, 255, 1024, 16384];
    for (const length of lengths) {
        const payload = Buffer.allocUnsafe(length);
        for (let index = 0; index < length; index++) {
            payload[index] = 0x20 + ((index * 29 + length) % 0x5f);
        }
        const [session] = native.createSession(0, length, '');
        assert.deepEqual(
            native.consume(session, clientFrame(payload, { opCode: 2 })),
            [[0, payload, true]],
            `incorrect unmask result for ${length} bytes`
        );
        native.dispose(session);
    }
});

test('validates UTF-8 around SIMD boundaries', () => {
    const offsets = [0, 15, 16, 17, 31, 32, 63, 64, 65, 79];
    for (const offset of offsets) {
        const validPayload = `${'a'.repeat(offset)}€${'b'.repeat(96)}`;
        const [validSession] = native.createSession(0, Buffer.byteLength(validPayload), '');
        assert.deepEqual(
            native.consume(validSession, clientFrame(validPayload)),
            [[0, validPayload, false]],
            `valid UTF-8 rejected at byte ${offset}`
        );
        native.dispose(validSession);

        const invalidPayload = Buffer.concat([
            Buffer.alloc(offset, 0x61),
            Buffer.from([0xc0, 0x80]),
            Buffer.alloc(96, 0x62)
        ]);
        const [invalidSession] = native.createSession(0, invalidPayload.length, '');
        const events = native.consume(invalidSession, clientFrame(invalidPayload));
        assert.equal(events[0][0], 1, `invalid UTF-8 accepted at byte ${offset}`);
        assert.deepEqual(events[1], [2, 1006, '']);
        native.dispose(invalidSession);
    }
});

test('rejects invalid UTF-8 after a large ASCII prefix', () => {
    const payload = Buffer.concat([
        Buffer.alloc(80, 0x61),
        Buffer.from([0xc0, 0x80]),
        Buffer.alloc(80, 0x62)
    ]);
    const [session] = native.createSession(0, payload.length, '');
    const events = native.consume(session, clientFrame(payload));

    assert.equal(events[0][0], 1);
    const closeFrame = parseServerFrame(events[0][1]);
    assert.equal(closeFrame.opCode, 8);
    assert.equal(closeFrame.payload.readUInt16BE(0), 1007);
    assert.deepEqual(events[1], [2, 1006, '']);
    native.dispose(session);
});

test('releases native events without invalidating a fragmented binary message', () => {
    const payload = Buffer.alloc(320 * 1024);
    for (let index = 0; index < payload.length; index++) payload[index] = index & 0xff;
    const frame = clientFrame(payload, { opCode: 2 });
    const split = Math.floor(frame.length / 2);
    const [session] = native.createSession(0, payload.length, '');

    assert.deepEqual(native.consume(session, frame.subarray(0, split)), []);
    const events = native.consume(session, frame.subarray(split));
    assert.equal(events.length, 1);
    assert.equal(events[0][0], 0);
    assert.equal(events[0][2], true);
    assert.deepEqual(events[0][1], payload);

    native.consume(session, Buffer.alloc(0));
    native.dispose(session);
    assert.deepEqual(events[0][1], payload);
});

test('handles fragmented messages with an interleaved ping', () => {
    const [session] = native.createSession(0, 1024, '');
    const events = native.consume(session, Buffer.concat([
        clientFrame('fragment-', { fin: false }),
        clientFrame('health', { opCode: 9 }),
        clientFrame('message', { opCode: 0 })
    ]));

    assert.equal(events.length, 2);
    assert.equal(events[0][0], 1);
    const pong = parseServerFrame(events[0][1]);
    assert.equal(pong.opCode, 10);
    assert.equal(pong.payload.toString(), 'health');
    assert.deepEqual(events[1], [0, 'fragment-message', false]);
    native.dispose(session);
});

test('rejects a new data frame after an interleaved control frame', () => {
    for (const opCode of [9, 10]) {
        const controlFrame = clientFrame('health', { opCode });
        const splitPoints = [null];
        for (let split = 1; split < controlFrame.length; split++) {
            splitPoints.push(split);
        }

        for (const split of splitPoints) {
            const [session] = native.createSession(0, 1024, '');
            const events = [];
            events.push(...native.consume(
                session,
                clientFrame('fragment-', { fin: false })
            ));
            if (split === null) {
                events.push(...native.consume(session, Buffer.from(controlFrame)));
            } else {
                events.push(...native.consume(
                    session,
                    Buffer.from(controlFrame.subarray(0, split))
                ));
                events.push(...native.consume(
                    session,
                    Buffer.from(controlFrame.subarray(split))
                ));
            }
            events.push(...native.consume(
                session,
                clientFrame('second-message', { opCode: 2 })
            ));

            assert.equal(
                events.some((event) => event[0] === 0),
                false,
                `accepted data after opcode ${opCode}, split ${split}`
            );
            const closeEvent = events.find((event) => event[0] === 2);
            assert.deepEqual(closeEvent, [2, 1006, '']);
            const closeFrame = events
                .filter((event) => event[0] === 1)
                .map((event) => parseServerFrame(event[1]))
                .find((frame) => frame.opCode === 8);
            assert.ok(closeFrame, 'missing protocol close frame');
            assert.equal(closeFrame.payload.readUInt16BE(0), 1002);
            native.dispose(session);
        }
    }
});

test('enforces maxPayload and treats zero as unlimited', () => {
    const limitedFrame = clientFrame('12345');
    const [limitedSession] = native.createSession(0, 4, '');
    const limitedEvents = native.consume(limitedSession, limitedFrame);
    assert.equal(limitedEvents[0][0], 1);
    assert.equal(parseServerFrame(limitedEvents[0][1]).payload.readUInt16BE(0), 1009);
    assert.deepEqual(limitedEvents[1], [2, 1006, '']);
    native.dispose(limitedSession);

    const [unlimitedSession] = native.createSession(0, 0, '');
    assert.deepEqual(
        native.consume(unlimitedSession, clientFrame('12345')),
        [[0, '12345', false]]
    );
    native.dispose(unlimitedSession);
});

test('uses close code 1009 when inflated data exceeds maxPayload', () => {
    const payload = 'a'.repeat(1000);
    const compressed = zlib.deflateRawSync(payload, {
        flush: zlib.constants.Z_SYNC_FLUSH,
        finishFlush: zlib.constants.Z_SYNC_FLUSH
    }).subarray(0, -4);
    const [session] = native.createSession(
        eiows.PERMESSAGE_DEFLATE,
        32,
        'permessage-deflate'
    );
    const events = native.consume(
        session,
        clientFrame(compressed, { compressed: true })
    );

    assert.equal(events[0][0], 1);
    assert.equal(parseServerFrame(events[0][1]).payload.readUInt16BE(0), 1009);
    assert.deepEqual(events[1], [2, 1006, '']);
    native.dispose(session);
});

test('echoes a valid close frame and reports its code and reason', () => {
    const [session] = native.createSession(0, 1024, '');
    const closePayload = Buffer.alloc(5);
    closePayload.writeUInt16BE(1000, 0);
    closePayload.write('bye', 2);
    const events = native.consume(session, clientFrame(closePayload, { opCode: 8 }));

    assert.equal(events[0][0], 1);
    const close = parseServerFrame(events[0][1]);
    assert.equal(close.opCode, 8);
    assert.equal(close.payload.readUInt16BE(0), 1000);
    assert.equal(close.payload.subarray(2).toString(), 'bye');
    assert.deepEqual(events[1], [2, 1000, 'bye']);
    native.dispose(session);
});

test('accepts current IANA close codes', () => {
    for (const code of [1012, 1013, 1014]) {
        const [session] = native.createSession(0, 1024, '');
        const payload = Buffer.alloc(2);
        payload.writeUInt16BE(code);
        const events = native.consume(session, clientFrame(payload, { opCode: 8 }));
        assert.deepEqual(events[1], [2, code, '']);
        native.dispose(session);
    }
});

test('rejects a new data frame during a fragmented message', () => {
    const [session] = native.createSession(0, 1024, '');
    const events = native.consume(session, Buffer.concat([
        clientFrame('text', { fin: false }),
        clientFrame(Buffer.from('binary'), { opCode: 2 })
    ]));

    assert.equal(events[0][0], 1);
    assert.equal(parseServerFrame(events[0][1]).payload.readUInt16BE(0), 1002);
    assert.deepEqual(events[1], [2, 1006, '']);
    native.dispose(session);
});

test('allows a control frame at the fragmented message payload limit', () => {
    const [session] = native.createSession(0, 4, '');
    const events = native.consume(session, Buffer.concat([
        clientFrame('1234', { fin: false }),
        clientFrame('x', { opCode: 9 }),
        clientFrame('', { opCode: 0 })
    ]));

    assert.equal(parseServerFrame(events[0][1]).opCode, 10);
    assert.deepEqual(events[1], [0, '1234', false]);
    native.dispose(session);
});

test('does not apply maxPayload to control frames', () => {
    const controlPayload = 'health';

    const [pingSession] = native.createSession(0, 4, '');
    const pingEvents = native.consume(
        pingSession,
        clientFrame(controlPayload, { opCode: 9 })
    );
    assert.equal(pingEvents.length, 1);
    const pong = parseServerFrame(pingEvents[0][1]);
    assert.equal(pong.opCode, 10);
    assert.equal(pong.payload.toString(), controlPayload);
    native.dispose(pingSession);

    const [pongSession] = native.createSession(0, 4, '');
    assert.deepEqual(
        native.consume(pongSession, clientFrame(controlPayload, { opCode: 10 })),
        []
    );
    native.dispose(pongSession);

    const closePayload = Buffer.alloc(5);
    closePayload.writeUInt16BE(1000, 0);
    closePayload.write('bye', 2);
    const [closeSession] = native.createSession(0, 4, '');
    const closeEvents = native.consume(
        closeSession,
        clientFrame(closePayload, { opCode: 8 })
    );
    const close = parseServerFrame(closeEvents[0][1]);
    assert.equal(close.opCode, 8);
    assert.equal(close.payload.readUInt16BE(0), 1000);
    assert.equal(close.payload.subarray(2).toString(), 'bye');
    assert.deepEqual(closeEvents[1], [2, 1000, 'bye']);
    native.dispose(closeSession);
});

test('waits for the peer close after initiating the close handshake', () => {
    const [session] = native.createSession(0, 1024, '');
    const closeFrame = native.closeFrame(session, 1000, Buffer.from('done'));
    assert.equal(parseServerFrame(closeFrame).opCode, 8);

    const peerPayload = Buffer.alloc(4);
    peerPayload.writeUInt16BE(1000, 0);
    peerPayload.write('ok', 2);
    assert.deepEqual(
        native.consume(session, clientFrame(peerPayload, { opCode: 8 })),
        [[2, 1000, 'ok']]
    );
    native.dispose(session);
});

test('writes uncompressed frames without copying their payload', async () => {
    const [session] = native.createSession(0, 1024 * 1024, '');
    const socket = new CapturingSocket();
    const webSocket = new eiows.WebSocket(session, socket, null, false, 1024, '', '');

    const mediumText = 'x'.repeat(126);
    await new Promise((resolve, reject) => {
        webSocket.send(mediumText, (error) => error ? reject(error) : resolve());
    });
    assert.equal(socket.writes[0].length, 4);
    assert.equal(socket.writes[0].readUInt16BE(2), 126);
    assert.equal(socket.writes[1], mediumText);

    await new Promise((resolve, reject) => {
        webSocket.send(mediumText, (error) => error ? reject(error) : resolve());
    });
    assert.strictEqual(socket.writes[2], socket.writes[0], 'expected cached text header');
    assert.equal(socket.writes[3], mediumText);

    const largeBinary = Buffer.alloc(65536, 0x61);
    await new Promise((resolve, reject) => {
        webSocket.send(largeBinary, (error) => error ? reject(error) : resolve());
    });
    assert.equal(socket.writes[4].length, 10);
    assert.equal(socket.writes[4].readBigUInt64BE(2), 65536n);
    assert.strictEqual(socket.writes[5], largeBinary);
    assert.equal(socket.corkCount, 3);

    const closed = new Promise((resolve) => webSocket.once('close', resolve));
    webSocket.terminate();
    await closed;
});

test('writes pre-encoded sendFrame parts without copying them', async () => {
    const [session] = native.createSession(0, 1024, '');
    const socket = new CapturingSocket();
    const webSocket = new eiows.WebSocket(session, socket, null, false, 1024, '', '');
    const header = Buffer.from([0x81, 0x05]);
    const payload = Buffer.from('hello');

    await new Promise((resolve, reject) => {
        webSocket._sender.sendFrame([header, payload], (error) =>
            error ? reject(error) : resolve());
    });
    assert.strictEqual(socket.writes[0], header);
    assert.strictEqual(socket.writes[1], payload);
    assert.equal(socket.corkCount, 1);

    const error = await new Promise((resolve) => {
        webSocket._sender.sendFrame([], resolve);
    });
    assert.match(error.message, /invalid pre-encoded frame/);

    const closed = new Promise((resolve) => webSocket.once('close', resolve));
    webSocket.terminate();
    await closed;
});

test('does not allocate zlib streams for every idle compression session', () => {
    const context = native.createCompressionContext();
    const options = eiows.PERMESSAGE_DEFLATE |
        eiows.CLIENT_NO_CONTEXT_TAKEOVER |
        eiows.SERVER_NO_CONTEXT_TAKEOVER;
    const sessions = [];
    const before = process.memoryUsage().rss;

    for (let index = 0; index < 2000; index++) {
        const [session] = native.createSession(
            options,
            1024,
            'permessage-deflate',
            context
        );
        sessions.push(session);
    }

    const increase = process.memoryUsage().rss - before;
    for (const session of sessions) native.dispose(session);
    assert.ok(
        increase < 64 * 1024 * 1024,
        `idle compression sessions increased RSS by ${Math.round(increase / 1024 / 1024)} MiB`
    );
});
