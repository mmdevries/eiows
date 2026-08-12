'use strict';

const http = require('node:http');
const eiows = require('../..');

const args = new Set(process.argv.slice(2));
const compressionEnabled = args.delete('--permessage-deflate');
if (args.size) {
    throw new Error(`unknown argument: ${args.values().next().value}`);
}

const httpServer = http.createServer();
const webSocketServer = new eiows.Server({
    maxPayload: 64 * 1024 * 1024,
    maxBackpressure: 128 * 1024 * 1024,
    perMessageDeflate: compressionEnabled ? { threshold: 0 } : false
});

httpServer.on('upgrade', (request, socket, head) => {
    webSocketServer.handleUpgrade(request, socket, head, (webSocket) => {
        webSocket.on('error', () => {});
        webSocket.on('message', (data, isBinary) => {
            webSocket.send(isBinary ? data : data.toString());
        });
    });
});

let shuttingDown = false;
function shutdown() {
    if (shuttingDown) return;
    shuttingDown = true;
    webSocketServer.close(() => httpServer.close(() => process.exit(0)));
    setTimeout(() => process.exit(1), 5000).unref();
}

process.once('SIGINT', shutdown);
process.once('SIGTERM', shutdown);
httpServer.listen(9001, '0.0.0.0', () => {
    process.stdout.write(
        `Autobahn server listening (permessage-deflate: ${compressionEnabled})\n`
    );
});
