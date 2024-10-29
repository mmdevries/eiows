eiows is a replacement module for ws which allows, but doesn't guarantee, significant performance and memory-usage improvements. This module is specifically only compatible with Node.js.
This package is mainly meant for projects which depend on the performance of the “original uws package”. This package requires engine.io(3.4.2 or higher) and it should work on Node 16, 18, 20, 22. Git should be installed on the system to build and compile the module.
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
