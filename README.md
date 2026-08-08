eiows is a replacement module for ws which allows, but doesn't guarantee, significant performance and memory-usage improvements. This module is specifically only compatible with Node.js.
This package is mainly meant for projects that want eiows as a high-performance `ws` replacement. It requires engine.io 3.4.2 or higher and supports Node.js 22 and newer. A C++17 compiler and zlib development headers are required when installing from source.

Starting with eiows 10, Node.js remains the owner of the underlying TCP or TLS stream. The native addon handles WebSocket framing, validation and compression through the stable Node-API. No Node.js source download or modification is required during installation.

eiows 10 exposes only the supported JavaScript API. The low-level native binding that older releases exposed as `eiows.native` is no longer public; native session handles are implementation details and cannot be used safely across API boundaries.

When `perMessageDeflate` is disabled, eiows also supports Engine.IO's `_sender.sendFrame()` fast path. Socket.IO can use this to reuse a single pre-encoded WebSocket frame for eligible text broadcasts, including room broadcasts, instead of framing and copying the payload separately for every recipient.
This module only runs on Linux/FreeBSD/macOS.

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

Releases are built by `.github/workflows/prebuilds.yml`. A version tag such as
`10.0.0` (or `v10.0.0`) must match `package.json`; the workflow builds and
tests every prebuild, assembles one npm tarball, verifies that tarball without
running install scripts on Node.js 22/24/26 with glibc and musl, and publishes
it using npm trusted publishing. Configure the npm trusted publisher for this
repository and the `prebuilds.yml` workflow, and allow the `npm publish`
action, before pushing the first release tag. A manual run builds the tarball
without publishing unless the `publish` input is enabled.


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
