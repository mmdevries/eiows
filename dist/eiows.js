/* jshint esversion: 6 */
/* jslint node: true, bitwise: true */

'use strict';
const EE_ERROR = 'Registering more than one listener to a WebSocket is not supported.';
const DEFAULT_PAYLOAD_LIMIT = 16777216;

var eiows = {};
eiows.PERMESSAGE_DEFLATE = 1;
eiows.SLIDING_DEFLATE_WINDOW = 16;
eiows.OPCODE_TEXT = 1;
eiows.OPCODE_BINARY = 2;
eiows.OPCODE_PING = 9;
eiows.OPEN = 1;
eiows.CLOSED = 0;

function noop() {}

function abortConnection(socket, code, message) {
    return socket.end(`HTTP/1.1 ${code} ${message}\r\n\r\n`);
}

const native = (() => {
    try {
        return require(`./eiows_${process.platform}_${process.versions.modules}`);
    } catch (e) {
        throw new Error(e.toString() + '\n\nCompilation of µWebSockets has failed and there is no correct pre-compiled binary ' +
            'available for your system or an unsupported node version is used. Please install a supported C++17 compiler or update node and reinstall the module \'eiows\'.');
    }
})();

native.setNoop(noop);

class WebSocket {
    constructor(external) {
        this.external = external;
        this.internalOnMessage = noop;
        this.internalOnClose = noop;
    }

    on(eventName, f) {
        if (eventName === 'message') {
            if (this.internalOnMessage !== noop) {
                throw Error(EE_ERROR);
            }
            this.internalOnMessage = f;
        }
        return this;
    }

    once(eventName, f) {
        if (eventName === 'close') {
            if (this.internalOnClose !== noop) {
                throw Error(EE_ERROR);
            }
            this.internalOnClose = (code, message) => {
                this.internalOnClose = noop;
                f(code, message);
            };
        }
        return this;
    }

    get _socket() {
        const address = this.external ? native.getAddress(this.external) : new Array(3);
        return {
            remotePort: address[0],
            remoteAddress: address[1],
            remoteFamily: address[2]
        };
    }

    removeListener() {
        return this;
    }

    send(message, options, cb) {
        if (this.external) {
            if (typeof options === 'function') {
                cb = options;
                options = null;
            }

            const binary = options && typeof options.binary === 'boolean' ? options.binary : typeof message !== 'string';

            native.server.send(this.external, message, binary ? eiows.OPCODE_BINARY : eiows.OPCODE_TEXT, cb ? (() => {
                process.nextTick(cb);
            }) : undefined, options && options.compress);
        } else if (cb) {
            cb(new Error('not opened'));
        }
    }

    close(code, data) {
        if (this.external) {
            native.server.close(this.external, code, data);
            this.external = null;
        }
    }
}

class Server {
    constructor(options) {
        if (!options) {
            throw new TypeError('missing options');
        }

        var nativeOptions = 0;
        if (options.perMessageDeflate !== undefined && options.perMessageDeflate !== false) {
            nativeOptions |= eiows.PERMESSAGE_DEFLATE;

            if (options.perMessageDeflate.serverNoContextTakeover === false) {
                nativeOptions |= eiows.SLIDING_DEFLATE_WINDOW;
            }
        }

        this.serverGroup = native.server.group.create(nativeOptions, options.maxPayload === undefined ? DEFAULT_PAYLOAD_LIMIT : options.maxPayload);

        this._upgradeCallback = noop;
        this._noDelay = options.noDelay === undefined ? true : options.noDelay;

        native.server.group.onDisconnection(this.serverGroup, (external, code, message, webSocket) => {
            webSocket.external = null;
            process.nextTick(() => {
                webSocket.internalOnClose(code, message);
            });
            native.clearUserData(external);
        });

        native.server.group.onMessage(this.serverGroup, (message, webSocket) => {
            webSocket.internalOnMessage(message);
        });

        native.server.group.onConnection(this.serverGroup, (external) => {
            const webSocket = new WebSocket(external);
            native.setUserData(external, webSocket);
            this._upgradeCallback(webSocket);
        });
    }

    handleUpgrade(request, socket, upgradeHead, callback) {
        const secKey = request.headers['sec-websocket-key'];
        const socketHandle = socket.ssl ? socket._parent._handle : socket._handle;
        if (socketHandle && secKey && secKey.length === 24) {
            const sslState = socket.ssl ? native.getSSLContext(socket.ssl) : null;
            socket.setNoDelay(this._noDelay);
            const ticket = native.transfer(socketHandle.fd === -1 ? socketHandle : socketHandle.fd, sslState);
            socket.on('close', () => {
                if (this.serverGroup) {
                    this._upgradeCallback = callback;
                    native.upgrade(this.serverGroup, ticket, secKey, request.headers['sec-websocket-extensions'], request.headers['sec-websocket-protocol']);
                }
            });
            setImmediate(() => {
                socket.destroy();
            });
        } else {
            return abortConnection(socket, 400, 'Bad Request');
        }
    }

    close() {
        if (this.serverGroup) {
            native.server.group.close(this.serverGroup);
            this.serverGroup = null;
        }
    }
}

eiows.Server = Server;
eiows.native = native;

module.exports = eiows;
