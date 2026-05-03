/* jshint esversion: 6 */
/* jslint node: true, bitwise: true */

'use strict';
const DEFAULT_PAYLOAD_LIMIT = 16777216;
const FastBuffer = Buffer[Symbol.species];

var eiows = {};
eiows.compressThreshold = 1024;
eiows.PERMESSAGE_DEFLATE = 1;
eiows.SERVER_NO_CONTEXT_TAKEOVER = 2;
eiows.CLIENT_NO_CONTEXT_TAKEOVER = 4;
eiows.SLIDING_DEFLATE_WINDOW = 16;
eiows.CONNECTING = 0;
eiows.OPCODE_TEXT = 1;
eiows.OPCODE_BINARY = 2;
eiows.OPCODE_PING = 9;
eiows.OPEN = 1;
eiows.CLOSING = 2;
eiows.CLOSED = 3;

function toBuffer(data) {
  if (Buffer.isBuffer(data)) return data;
  let buf;
  if (data instanceof ArrayBuffer) {
    buf = new FastBuffer(data);
  } else if (ArrayBuffer.isView(data)) {
    buf = new FastBuffer(data.buffer, data.byteOffset, data.byteLength);
  } else {
    buf = Buffer.from(data);
  }
  return buf;
}

function getMessageByteLength(message, binary) {
  if (!binary) {
    return Buffer.byteLength(message);
  }
  if (Buffer.isBuffer(message)) {
    return message.length;
  }
  if (message instanceof ArrayBuffer || ArrayBuffer.isView(message)) {
    return message.byteLength;
  }
  return toBuffer(message).length;
}

function noop() {}

function abortConnection(socket, code, message) {
    return socket.end(`HTTP/1.1 ${code} ${message}\r\n\r\n`);
}

function hasHeaderToken(value, token) {
    return typeof value === 'string' && value
        .split(',')
        .some((part) => part.trim().toLowerCase() === token);
}

function isValidSecWebSocketKey(value) {
    return typeof value === 'string' && value.length === 24 && /^[A-Za-z0-9+/]{22}==$/.test(value);
}

const native = (() => {
    try {
        return require(`./eiows_${process.version}`);
    } catch (e) {
        throw new Error(e.toString() + '\n\nCompilation of eiows has failed.' +
            'Please install a supported C++17 compiler, git and/or update node and reinstall the module \'eiows\'.');
    }
})();

native.setNoop(noop);

class WebSocket {
    constructor(external, compressEnabled = false, compressThreshold = eiows.compressThreshold) {
        this.external = external;
        this.CONNECTING = eiows.CONNECTING;
        this.OPEN = eiows.OPEN;
        this.CLOSING = eiows.CLOSING;
        this.CLOSED = eiows.CLOSED;
        this.readyState = external ? eiows.OPEN : eiows.CLOSED;
        this.compressEnabled = compressEnabled;
        this.compressThreshold = compressThreshold;
        this._listeners = Object.create(null);
        this.internalOnMessage = (message, isBinary) => {
            this._emit('message', message, isBinary);
        };
        this.internalOnClose = (code, message) => {
            this._emit('close', code, message);
        };
    }

    on(eventName, f) {
        this._addListener(eventName, f);
        return this;
    }

    once(eventName, f) {
        this._addListener(eventName, f, true);
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

    removeListener(eventName, f) {
        const listeners = this._listeners[eventName];
        if (!listeners || !listeners.length) {
            return this;
        }

        if (typeof f !== 'function') {
            return this;
        }

        const index = listeners.findIndex((listener) => listener === f || listener._originalListener === f);
        if (index !== -1) {
            listeners.splice(index, 1);
        }
        return this;
    }

    send(message, options, cb) {
        if (typeof options === 'function') {
            cb = options;
            options = undefined;
        }

        if (this.external && this.readyState === eiows.OPEN) {
            const binary = (typeof message !== 'string');
            var compress = false;
            if (this.compressEnabled && (!options || options.compress !== false)) {
                var byteLength = getMessageByteLength(message, binary);
                if (byteLength >= this.compressThreshold) {
                    compress = true;
                }
            }
            native.server.send(this.external, message, binary ? eiows.OPCODE_BINARY : eiows.OPCODE_TEXT, cb, compress);
        } else if (cb) {
            cb(new Error('not opened'));
        }
    }

    close(code, data) {
        if (this.readyState === eiows.CLOSING || this.readyState === eiows.CLOSED) {
            return;
        }

        if (this.external) {
            this.readyState = eiows.CLOSING;
            native.server.close(this.external, code, data);
        } else {
            this.readyState = eiows.CLOSED;
        }
    }

    _addListener(eventName, listener, once = false) {
        if (typeof listener !== 'function') {
            return;
        }

        const listeners = this._listeners[eventName] || (this._listeners[eventName] = []);
        if (once) {
            const wrappedListener = (...args) => {
                this.removeListener(eventName, wrappedListener);
                listener(...args);
            };
            wrappedListener._originalListener = listener;
            listeners.push(wrappedListener);
        } else {
            listeners.push(listener);
        }
    }

    _emit(eventName, ...args) {
        const listeners = this._listeners[eventName];
        if (!listeners || !listeners.length) {
            return;
        }

        for (const listener of listeners.slice()) {
            listener(...args);
        }
    }
}

class Server {
    constructor(options) {
        if (!options) {
            throw new TypeError('missing options');
        }

        this._compressEnabled = false;
        this._compressThreshold = eiows.compressThreshold;
        var nativeOptions = 0;
        if (options.perMessageDeflate !== undefined && options.perMessageDeflate !== false) {
            const perMessageDeflateOptions = options.perMessageDeflate === true ? {} : options.perMessageDeflate;
            nativeOptions |= eiows.PERMESSAGE_DEFLATE;
            this._compressEnabled = true;
            if (!isNaN(perMessageDeflateOptions.threshold) && perMessageDeflateOptions.threshold >= 0) {
                this._compressThreshold = perMessageDeflateOptions.threshold;
            }

            if (perMessageDeflateOptions.clientNoContextTakeover !== false) {
                nativeOptions |= eiows.CLIENT_NO_CONTEXT_TAKEOVER;
            }

            if (perMessageDeflateOptions.serverNoContextTakeover !== false) {
                nativeOptions |= eiows.SERVER_NO_CONTEXT_TAKEOVER;
            } else {
                nativeOptions |= eiows.SLIDING_DEFLATE_WINDOW;
            }
        }

        this.serverGroup = native.server.group.create(nativeOptions, options.maxPayload === undefined ? DEFAULT_PAYLOAD_LIMIT : options.maxPayload);

        this._pendingUpgradeCallbacks = [];
        this._noDelay = options.noDelay === undefined ? true : options.noDelay;

        native.server.group.onDisconnection(this.serverGroup, (external, code, message, webSocket) => {
            webSocket.external = null;
            webSocket.readyState = eiows.CLOSED;
            process.nextTick(() => {
                webSocket.internalOnClose(code, message);
            });
            native.clearUserData(external);
        });

        native.server.group.onMessage(this.serverGroup, (message, webSocket, isBinary) => {
            webSocket.internalOnMessage(message, isBinary);
        });

        native.server.group.onConnection(this.serverGroup, (external) => {
            const webSocket = new WebSocket(external, this._compressEnabled, this._compressThreshold);
            native.setUserData(external, webSocket);
            const upgradeCallback = this._pendingUpgradeCallbacks.shift() || noop;
            upgradeCallback(webSocket);
        });
    }

    handleUpgrade(request, socket, upgradeHead, callback) {
        const secKey = request.headers['sec-websocket-key'];
        const connectionHeader = request.headers.connection;
        const upgradeHeader = request.headers.upgrade;
        const version = request.headers['sec-websocket-version'];
        const socketHandle = socket.ssl ? socket._parent._handle : socket._handle;
        const hasUpgradeHead = upgradeHead && upgradeHead.length > 0;
        const validUpgrade = socketHandle &&
            request.method === 'GET' &&
            !hasUpgradeHead &&
            isValidSecWebSocketKey(secKey) &&
            version === '13' &&
            typeof upgradeHeader === 'string' &&
            upgradeHeader.toLowerCase() === 'websocket' &&
            hasHeaderToken(connectionHeader, 'upgrade');

        if (validUpgrade) {
            const sslState = socket.ssl ? native.getSSLContext(socket.ssl) : null;
            socket.setNoDelay(this._noDelay);
            const ticket = native.transfer(socketHandle.fd === -1 ? socketHandle : socketHandle.fd, sslState);
            socket.on('close', () => {
                if (this.serverGroup) {
                    this._pendingUpgradeCallbacks.push(callback);
                    try {
                        if (!native.upgrade(this.serverGroup, ticket, secKey, request.headers['sec-websocket-extensions'], request.headers['sec-websocket-protocol'])) {
                            this._pendingUpgradeCallbacks.pop();
                        }
                    } catch (err) {
                        this._pendingUpgradeCallbacks.pop();
                        throw err;
                    }
                } else {
                    native.destroyTicket(ticket);
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
