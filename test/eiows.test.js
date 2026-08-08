'use strict';

const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const net = require('node:net');
const path = require('node:path');
const tls = require('node:tls');
const test = require('node:test');
const zlib = require('node:zlib');

const eiows = require('..');
const native = require('node-gyp-build')(path.join(__dirname, '..'));

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
        assert.ok(buffer.length >= 4, 'incomplete 16-bit frame header');
        payloadLength = buffer.readUInt16BE(2);
        offset = 4;
    } else if (payloadLength === 127) {
        assert.ok(buffer.length >= 10, 'incomplete 64-bit frame header');
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

async function runEchoCase(secure) {
    const server = secure ? https.createServer(tlsOptions) : http.createServer();
    const wsServer = new eiows.Server({ maxPayload: 1024 });
    const messages = [];
    wsServer.on('headers', (headers) => headers.push('X-Eiows-Test: yes'));
    server.on('upgrade', (request, socket, head) => {
        wsServer.handleUpgrade(request, socket, head, (webSocket, callbackRequest) => {
            assert.equal(callbackRequest, request);
            webSocket.on('error', () => {});
            webSocket.on('message', (message, isBinary) => {
                messages.push([message, isBinary]);
                if (secure) {
                    webSocket.send(message);
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
    assert.deepEqual(messages, [['hello', false]]);

    await destroySocket(socket);
    await new Promise((resolve) => wsServer.close(resolve));
    await closeServer(server);
}

test('keeps the HTTP socket in Node and consumes upgradeHead', () => runEchoCase(false));
test('keeps the TLS socket in Node without accessing SSLPointer', () => runEchoCase(true));

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

    const largeBinary = Buffer.alloc(65536, 0x61);
    await new Promise((resolve, reject) => {
        webSocket.send(largeBinary, (error) => error ? reject(error) : resolve());
    });
    assert.equal(socket.writes[2].length, 10);
    assert.equal(socket.writes[2].readBigUInt64BE(2), 65536n);
    assert.strictEqual(socket.writes[3], largeBinary);
    assert.equal(socket.corkCount, 2);

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
