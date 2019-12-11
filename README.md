uws is a replacement module for ws which allows, but doesn't guarantee, significant performance and memory-usage improvements. This module is specifically only compatible with Node.js.
This package is mainly meant for projects which depend on the performance of the “original uws package” in combination with Socket.IO and Express and it should work on Node 8, 10 and 12.

Installation:

npm install mmdevries/uws#2.4.0

or

yarn add mmdevries/uws#2.4.0


Example:

    var fs = require('fs');
    var https = require('https');
    var express = require('express');

    var ssl_options = {
        key: fs.readFileSync('server.key'),
        cert: fs.readFileSync('server.crt'),
    };

    var app = express();
    var server = https.createServer(ssl_options, app);

    server.listen(1443);

    var io = require("socket.io")(server, {
        wsEngine: 'uws',
        perMessageDeflate: {
            threshold: 32768,
            serverNoContextTakeover: false
        }
    });

    io.on("connection", function(socket) {
        console.log('Yes, you did it!');
    });


Have fun!
