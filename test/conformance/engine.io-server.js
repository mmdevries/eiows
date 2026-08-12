'use strict';

const http = require('node:http');
const { Server } = require('engine.io');
const eiows = require('../..');

const httpServer = http.createServer();
const engine = new Server({
    wsEngine: eiows.Server,
    pingInterval: 300,
    pingTimeout: 200,
    maxHttpBufferSize: 1e6,
    cors: { origin: '*' }
});

engine.attach(httpServer);
engine.on('connection', (socket) => {
    socket.on('message', (data) => socket.send(data));
});

let shuttingDown = false;
function shutdown() {
    if (shuttingDown) return;
    shuttingDown = true;
    engine.close();
    httpServer.close(() => process.exit(0));
    setTimeout(() => process.exit(1), 5000).unref();
}

process.once('SIGINT', shutdown);
process.once('SIGTERM', shutdown);
httpServer.listen(3000, () => {
    process.stdout.write('engine.io conformance server listening\n');
});
