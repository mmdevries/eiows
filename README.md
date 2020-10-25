eiows is a replacement module for ws which allows, but doesn't guarantee, significant performance and memory-usage improvements. This module is specifically only compatible with Node.js.
This package is mainly meant for projects which depend on the performance of the “original uws package”. This package requires engine.io(3.4.2 or higher) and it should work on Node 8, 10, 12, 13(v13.14.0 or higher), 14(v14.4.0 or higher) and 15.

Installation:

npm install eiows

or

yarn add eiows


Example:

    var fs = require('fs');
    var https = require('https');

    var ssl_options = {
        key: fs.readFileSync('server.key'),
        cert: fs.readFileSync('server.crt'),
    };

    var server = https.createServer(ssl_options);

    server.listen(1443);

    var io = require("socket.io")(server, {
        wsEngine: 'eiows',
        perMessageDeflate: {
            threshold: 32768
        }
    });

    io.on("connection", function(socket) {
        console.log('Yes, you did it!');
    });


Have fun!
