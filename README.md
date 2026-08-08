eiows is a replacement module for ws which allows, but doesn't guarantee, significant performance and memory-usage improvements. This module is specifically only compatible with Node.js.
This package is mainly meant for projects that want eiows as a high-performance `ws` replacement. It requires engine.io 3.4.2 or higher and supports Node.js 16 and newer. A C++17 compiler is required when installing from source.

Starting with eiows 10, Node.js remains the owner of the underlying TCP or TLS stream. The native addon handles WebSocket framing, validation and compression through the stable Node-API. No Node.js source download or modification is required during installation.

When `perMessageDeflate` is disabled, eiows also supports Engine.IO's `_sender.sendFrame()` fast path. Socket.IO can use this to reuse a single pre-encoded WebSocket frame for eligible text broadcasts, including room broadcasts, instead of framing and copying the payload separately for every recipient.
This module only runs on Linux/FreeBSD/MacOS.

Installation:

npm install eiows

or

yarn add eiows


Examples:

    // ESM
    import * as http from 'http';
    import { Server } from "socket.io";
    import eiows from 'eiows';

    let server = http.createServer();

    let io = new Server(server, {
        wsEngine: eiows.Server,
        perMessageDeflate: {
            threshold: 32768
        }
    });

    io.on("connection", () => {
        console.log('Yes, you did it!');
    });
    server.listen(8080);

    // CJS
    var http = require('http');
    var server = http.createServer();

    var io = require("socket.io")(server, {
        wsEngine: require("eiows").Server,
        perMessageDeflate: {
            threshold: 32768
        }
    });

    io.on("connection", function(socket) {
        console.log('Yes, you did it!');
    });
    server.listen(8080);

Have fun!
