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

Release packages are built manually with `.github/workflows/prebuilds.yml`.
After updating the version in `package.json`, commit the release and push a tag
matching that version on the same commit, such as `10.0.0` or `v10.0.0`. Run
the workflow with that tag selected on the GitHub Actions page; no release
input is required. The workflow requires a tag ref, verifies that it matches
the package version, then builds every prebuild from the tag's exact commit.
It assembles one npm tarball and verifies that tarball without running install
scripts on Node.js 22/24/26 with glibc and musl. Download the `npm-package`
workflow artifact and publish the contained tarball manually with
`npm publish eiows-*.tgz --access public`.


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
