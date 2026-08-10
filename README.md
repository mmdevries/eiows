eiows is a high-performance `ws` replacement for Node.js and Engine.IO. It
supports the maintained Node.js 22, 24 and 26 release lines on Linux, FreeBSD
and macOS.

After a successful HTTP upgrade, eiows takes ownership of the live transport.
It duplicates the socket descriptor, stops and destroys Node's TCPWrap or
TLSWrap, and drives the connection with its own `uv_poll_t`. TCP ingress uses a
thread-local read buffer and TCP egress uses `sendmsg()` vectors. For TLS, the
addon takes a reference to the negotiated `SSL*` and its BIO queues before Node
teardown, then drives those queues itself and batches encrypted records onto the
owned descriptor. Activation waits until Node has released its TLSWrap SSL
reference. The active WebSocket does not retain the original Node socket object
graph.

This implementation deliberately uses version-specific Node.js internals; a
single Node-API prebuild cannot safely provide this ownership model. Install
therefore compiles against the exact running Node.js source release. The build
downloads the official source archive on first use, verifies it against the
release SHA-256 manifest and caches it. Set `EIOWS_NODE_SOURCE_DIR` to a complete
matching source tree for offline or hermetic builds, and
`EIOWS_NODE_SOURCE_CACHE` to choose a different cache directory. A C++20
compiler, Python, make and zlib development headers are required.

The compiled addon is stored as `dist/eiows_ABI.node`, where `ABI` is the
running Node.js module ABI number. The loader never falls back to a binary for
another ABI, and the addon also rejects an exact Node.js release mismatch before
initializing any version-specific Node or V8 internals. Rebuild eiows after every
Node.js upgrade, including patch releases.

eiows 11 exposes only the supported JavaScript API. The low-level native binding that older releases exposed as `eiows.native` is no longer public; native session handles are implementation details and cannot be used safely across API boundaries.

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

The private `_socket` compatibility property exposes copied peer address
metadata after takeover; it is not the destroyed Node `Duplex`. Supported
application I/O goes through `send()`, `close()`, `terminate()` and WebSocket
events.

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
