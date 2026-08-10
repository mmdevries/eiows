'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const net = require('node:net');
const path = require('node:path');
const tls = require('node:tls');
const { spawnSync } = require('node:child_process');
const { createRequire } = require('node:module');

const installRoot = process.argv[2] && path.resolve(process.argv[2]);
if (!installRoot) throw new Error('installed package root is required');

const requireFromInstall = createRequire(path.join(installRoot, 'package.json'));
const eiows = requireFromInstall('eiows');
const packageRoot = path.dirname(requireFromInstall.resolve('eiows/package.json'));
const binaryName = `eiows_${process.versions.modules}.node`;

assert.equal(typeof eiows.Server, 'function');
assert.equal(Object.hasOwn(eiows, 'native'), false);
assert.equal(fs.existsSync(path.join(packageRoot, 'dist', binaryName)), true);
assert.equal(fs.existsSync(path.join(packageRoot, 'dist', 'eiows.node')), false);

const esmProbe = spawnSync(process.execPath, [
    '--input-type=module',
    '--eval',
    'import eiows, { Server } from "eiows"; ' +
        'if (Server !== eiows.Server) throw new Error("ESM export mismatch"); ' +
        'const server = new Server({ perMessageDeflate: false }); ' +
        'await new Promise((resolve) => server.close(resolve));'
], {
    cwd: installRoot,
    encoding: 'utf8',
    timeout: 30000
});
assert.equal(
    esmProbe.status,
    0,
    `packed ESM probe failed:\n${esmProbe.stdout}${esmProbe.stderr}`
);

const fixtures = path.join(__dirname, 'fixtures');
const tlsOptions = {
    key: fs.readFileSync(path.join(fixtures, 'key.pem')),
    cert: fs.readFileSync(path.join(fixtures, 'cert.pem'))
};

function clientFrame(value) {
    const payload = Buffer.from(value);
    const mask = Buffer.from([0x12, 0x34, 0x56, 0x78]);
    const frame = Buffer.alloc(6 + payload.length);
    frame[0] = 0x81;
    frame[1] = 0x80 | payload.length;
    mask.copy(frame, 2);
    for (let index = 0; index < payload.length; index++) {
        frame[6 + index] = payload[index] ^ mask[index & 3];
    }
    return frame;
}

function upgradeRequest() {
    return 'GET / HTTP/1.1\r\n' +
        'Host: localhost\r\n' +
        'Upgrade: websocket\r\n' +
        'Connection: Upgrade\r\n' +
        'Sec-WebSocket-Version: 13\r\n' +
        'Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==\r\n\r\n';
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
    return new Promise((resolve, reject) => {
        server.close((error) => error ? reject(error) : resolve());
    });
}

function waitForEcho(socket) {
    return new Promise((resolve, reject) => {
        let received = Buffer.alloc(0);
        const timeout = setTimeout(
            () => reject(new Error('timed out waiting for packed WebSocket echo')),
            5000
        );
        const finish = (error, value) => {
            clearTimeout(timeout);
            socket.removeListener('data', onData);
            socket.removeListener('error', onError);
            socket.removeListener('close', onClose);
            if (error) reject(error);
            else resolve(value);
        };
        const onError = (error) => finish(error);
        const onClose = () => finish(new Error('socket closed before packed WebSocket echo'));
        const onData = (chunk) => {
            received = Buffer.concat([received, chunk]);
            const headerEnd = received.indexOf('\r\n\r\n');
            if (headerEnd === -1) return;
            assert.match(received.subarray(0, headerEnd).toString(),
                /^HTTP\/1\.1 101 Switching Protocols/m);
            const frame = received.subarray(headerEnd + 4);
            if (frame.length < 2) return;
            const length = frame[1] & 0x7f;
            if (length >= 126 || frame.length < 2 + length) return;
            assert.equal(frame[0] & 0x0f, 1);
            finish(null, frame.subarray(2, 2 + length).toString());
        };
        socket.on('data', onData);
        socket.once('error', onError);
        socket.once('close', onClose);
    });
}

async function runUpgradeSmoke(secure) {
    const server = secure ? https.createServer(tlsOptions) : http.createServer();
    const webSocketServer = new eiows.Server({ perMessageDeflate: false });
    let socket;
    server.on('upgrade', (request, transport, head) => {
        webSocketServer.handleUpgrade(request, transport, head, (webSocket) => {
            assert.equal(webSocket._nativeTransport, true);
            webSocket.on('error', () => {});
            // Text messages are exposed as Buffers by default for Engine.IO.
            // Convert the echo back to a string so the outbound text path is
            // covered as well as the inbound Buffer-compatible path.
            webSocket.on('message', (message) => webSocket.send(message.toString()));
        });
    });

    try {
        const port = await listen(server);
        socket = secure
            ? tls.connect({ port, host: '127.0.0.1', rejectUnauthorized: false })
            : net.connect({ port, host: '127.0.0.1' });
        await new Promise((resolve, reject) => {
            socket.once(secure ? 'secureConnect' : 'connect', resolve);
            socket.once('error', reject);
        });
        const echoed = waitForEcho(socket);
        socket.write(Buffer.concat([Buffer.from(upgradeRequest()), clientFrame('packed')]));
        assert.equal(await echoed, 'packed');
    } finally {
        if (socket && !socket.destroyed) socket.destroy();
        await new Promise((resolve) => webSocketServer.close(resolve));
        if (server.listening) await closeServer(server);
    }
}

(async () => {
    await runUpgradeSmoke(false);
    await runUpgradeSmoke(true);
    console.log(`packed eiows smoke passed with ${binaryName}`);
})().catch((error) => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
