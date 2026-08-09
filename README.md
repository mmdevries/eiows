eiows is a replacement module for ws which allows, but doesn't guarantee, significant performance and memory-usage improvements. This module is specifically only compatible with Node.js.
This package is mainly meant for projects that want eiows as a high-performance `ws` replacement. It requires engine.io 3.4.2 or higher and supports Node.js 22 and newer. A C++17 compiler and zlib development headers are required when installing from source.

Starting with eiows 10, Node.js remains the owner of the underlying TCP or TLS stream. The native addon handles WebSocket framing, validation and compression through the stable Node-API. No Node.js source download or modification is required during installation.

eiows 10 exposes only the supported JavaScript API. The low-level native binding that older releases exposed as `eiows.native` is no longer public; native session handles are implementation details and cannot be used safely across API boundaries.

When `perMessageDeflate` is disabled, eiows also supports Engine.IO's `_sender.sendFrame()` fast path. Socket.IO can use this to reuse a single pre-encoded WebSocket frame for eligible text broadcasts, including room broadcasts, instead of framing and copying the payload separately for every recipient.
This module only runs on Linux/FreeBSD/macOS.

Engine.IO integration
---------------------

The regular `Server` delivers validated text frames as `Buffer` values with
`isBinary === false`, matching the interface Engine.IO expects from `ws`.
Engine.IO then performs its normal synchronous text conversion. Existing
Engine.IO and Socket.IO configurations therefore receive the optimized path
without code changes, including for Unicode-heavy traffic.

Direct consumers that depend on the eiows 10.0.1 behavior of receiving a
JavaScript string can temporarily opt back in with `textAsString: true`.

Published packages include Node-API prebuilds for:

- Linux x64 and arm64 (both glibc and musl/Alpine)
- macOS x64 and arm64
- FreeBSD x64 and arm64

The same Node-API binary is compatible with Node.js 22, 24 and 26. If no
matching prebuild is available, installation falls back to a local source
build and requires a C++17 compiler and zlib development headers.

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
        perMessageDeflate: false
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
        perMessageDeflate: false
    });

    io.on("connection", function(socket) {
        console.log('Yes, you did it!');
    });
    server.listen(8080);

Have fun!
