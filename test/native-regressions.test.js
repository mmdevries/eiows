'use strict';

const assert = require('node:assert/strict');
const { once } = require('node:events');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const net = require('node:net');
const path = require('node:path');
const tls = require('node:tls');
const test = require('node:test');
const eiows = require('..');

const tlsOptions = {
    key: fs.readFileSync(path.join(__dirname, 'fixtures/key.pem')),
    cert: fs.readFileSync(path.join(__dirname, 'fixtures/cert.pem'))
};

function clientCloseFrame() {
    const frame = Buffer.from([0x88, 0x82, 0x12, 0x34, 0x56, 0x78, 0, 0]);
    frame[6] = 0x03 ^ frame[2];
    frame[7] = 0xe8 ^ frame[3];
    return frame;
}

async function upgradedConnection(t, secure, compression = false) {
    const server = secure ? https.createServer(tlsOptions) : http.createServer();
    const wsServer = new eiows.Server({
        perMessageDeflate: compression ? { threshold: 0 } : false
    });
    let client;
    t.after(async () => {
        if (client) client.destroy();
        await new Promise((resolve) => wsServer.close(resolve));
        await new Promise((resolve) => server.close(resolve));
    });

    const upgraded = new Promise((resolve) => {
        server.on('upgrade', (request, socket, head) => {
            wsServer.handleUpgrade(request, socket, head, (webSocket) => {
                webSocket.on('error', () => {});
                resolve(webSocket);
            });
        });
    });
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const options = { port: server.address().port, host: '127.0.0.1' };
    client = secure
        ? tls.connect({ ...options, rejectUnauthorized: false })
        : net.connect(options);
    await once(client, secure ? 'secureConnect' : 'connect');
    client.pause();
    client.write('GET / HTTP/1.1\r\nHost: localhost\r\n' +
        'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
        'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n' +
        'Sec-WebSocket-Version: 13\r\n' +
        (compression ? 'Sec-WebSocket-Extensions: permessage-deflate\r\n' : '') + '\r\n');
    const webSocket = await upgraded;
    assert.equal(webSocket._nativeActive, true);
    return { client, webSocket };
}

async function blockedConnection(t, secure) {
    const { client, webSocket } = await upgradedConnection(t, secure);
    const payload = Buffer.alloc(16 * 1024 * 1024, 0x61);
    const writeCompleted = new Promise((resolve) => {
        webSocket.send(payload, (error) => resolve(error));
    });
    assert.ok(webSocket.bufferedAmount > 0, 'expected a blocked output queue');
    return { client, webSocket, writeCompleted, payload };
}

function waitForPeerClose(webSocket) {
    return new Promise((resolve) => {
        const onClose = webSocket._onNativeClose;
        webSocket._onNativeClose = function (code, reason) {
            onClose.call(this, code, reason);
            resolve();
        };
    });
}

for (const secure of [false, true]) {
    const transport = secure ? 'TLS' : 'TCP';

    for (const compression of [false, true]) {
        test(`native ${transport} delivers a message before its batched peer close (${compression ? 'compression enabled' : 'uncompressed'})`,
        { timeout: 5000 }, async (t) => {
            const { client, webSocket } = await upgradedConnection(t, secure, compression);
            assert.equal(webSocket.compressEnabled, compression);
            const errors = [];
            webSocket.on('error', (error) => errors.push(error));
            webSocket.on('message', (message) => webSocket.send(message.toString()));
            const closed = once(webSocket, 'close');
            const received = [];
            client.on('data', (chunk) => received.push(chunk));
            const ended = once(client, 'end');
            const text = Buffer.from([0x81, 0x84, 0x12, 0x34, 0x56, 0x78,
                0x65 ^ 0x12, 0x63 ^ 0x34, 0x68 ^ 0x56, 0x6f ^ 0x78]);
            client.write(Buffer.concat([text, clientCloseFrame()]));
            client.resume();
            const [code] = await closed;
            await ended;
            assert.deepEqual(errors, []);
            assert.equal(code, 1000);
            const response = Buffer.concat(received);
            const headerEnd = response.indexOf('\r\n\r\n') + 4;
            assert.ok(headerEnd > 3);
            if (compression) {
                assert.match(response.subarray(0, headerEnd).toString(),
                    /Sec-WebSocket-Extensions: permessage-deflate/i);
            }
            assert.deepEqual(response.subarray(headerEnd), Buffer.from([
                0x81, 0x04, 0x65, 0x63, 0x68, 0x6f, 0x88, 0x02, 0x03, 0xe8
            ]));
        });
    }

    test(`native ${transport} starts the peer-close deadline behind blocked writes`,
        { timeout: 5000 }, async (t) => {
            const { client, webSocket, writeCompleted } = await blockedConnection(t, secure);
            const peerClose = waitForPeerClose(webSocket);
            const closed = once(webSocket, 'close');
            t.mock.timers.enable({ apis: ['setTimeout'] });
            client.write(clientCloseFrame());
            await peerClose;

            assert.equal(webSocket.readyState, eiows.CLOSING);
            assert.ok(webSocket._closeTimer, 'peer close must start its timeout immediately');
            assert.ok(webSocket.bufferedAmount > 0, 'close reply must still be behind queued data');
            const queued = webSocket.bufferedAmount;
            const sendError = await new Promise((resolve) => webSocket.send('too late', resolve));
            assert.match(sendError.message, /not open/);
            assert.ok(webSocket.bufferedAmount <= queued);

            t.mock.timers.tick(15000);
            await closed;
            assert.ok(await writeCompleted, 'deadline must cancel the unfinished write');
            assert.equal(webSocket.bufferedAmount, 0);
        });

    test(`native ${transport} drains queued data and its close reply before shutdown`,
        { timeout: 10000 }, async (t) => {
            const { client, webSocket, writeCompleted, payload } =
                await blockedConnection(t, secure);
            const peerClose = waitForPeerClose(webSocket);
            const closed = once(webSocket, 'close');
            client.write(clientCloseFrame());
            await peerClose;
            assert.equal(webSocket.readyState, eiows.CLOSING);
            assert.ok(webSocket.bufferedAmount > 0);

            const received = [];
            client.on('data', (chunk) => received.push(chunk));
            const ended = once(client, 'end');
            client.resume();
            const [code] = await closed;
            await ended;
            assert.equal(code, 1000);
            assert.equal(await writeCompleted, undefined);

            const response = Buffer.concat(received);
            const headerEnd = response.indexOf('\r\n\r\n') + 4;
            assert.ok(headerEnd > 3, 'expected the upgrade response');
            const frame = response.subarray(headerEnd);
            assert.equal(frame[0], 0x82);
            assert.equal(frame[1], 127);
            assert.equal(frame.readBigUInt64BE(2), BigInt(payload.length));
            assert.deepEqual(frame.subarray(10, 10 + payload.length), payload);
            assert.deepEqual(frame.subarray(10 + payload.length),
                Buffer.from([0x88, 0x02, 0x03, 0xe8]));
        });
}
