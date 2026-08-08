'use strict';

const { createHash } = require('node:crypto');
const { EventEmitter } = require('node:events');

const DEFAULT_PAYLOAD_LIMIT = 16777216;
const CLOSE_TIMEOUT = 15000;
const FastBuffer = Buffer[Symbol.species];

const eiows = {};
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

const native = (() => {
    try {
        return require('./eiows.node');
    } catch (error) {
        throw new Error(error.toString() + '\n\nCompilation of eiows has failed. ' +
            'Please install a supported C++17 compiler and rebuild the module.');
    }
})();

function toBuffer(data) {
    if (Buffer.isBuffer(data)) return data;
    if (data instanceof ArrayBuffer) {
        return new FastBuffer(data);
    }
    if (ArrayBuffer.isView(data)) {
        return new FastBuffer(data.buffer, data.byteOffset, data.byteLength);
    }
    return Buffer.from(data);
}

function getMessageByteLength(message, binary) {
    if (!binary) return Buffer.byteLength(message);
    if (Buffer.isBuffer(message)) return message.length;
    if (message instanceof ArrayBuffer || ArrayBuffer.isView(message)) {
        return message.byteLength;
    }
    return toBuffer(message).length;
}

function createFrameHeader(payloadLength, opCode) {
    if (payloadLength < 126) {
        return Buffer.from([0x80 | opCode, payloadLength]);
    }
    if (payloadLength <= 0xffff) {
        const header = Buffer.allocUnsafe(4);
        header[0] = 0x80 | opCode;
        header[1] = 126;
        header.writeUInt16BE(payloadLength, 2);
        return header;
    }
    const header = Buffer.allocUnsafe(10);
    header[0] = 0x80 | opCode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(payloadLength), 2);
    return header;
}

function headerValue(value) {
    if (Array.isArray(value)) return value.join(', ');
    return typeof value === 'string' ? value : '';
}

function hasHeaderToken(value, token) {
    return headerValue(value)
        .split(',')
        .some((part) => part.trim().toLowerCase() === token);
}

function isValidSecWebSocketKey(value) {
    return typeof value === 'string' &&
        value.length === 24 &&
        /^[A-Za-z0-9+/]{22}==$/.test(value);
}

function isValidCloseCode(code) {
    return (code >= 1000 && code <= 1014 &&
        code !== 1004 && code !== 1005 && code !== 1006) ||
        (code >= 3000 && code <= 4999);
}

function isValidUtf8(buffer) {
    return Buffer.from(buffer.toString('utf8'), 'utf8').equals(buffer);
}

function selectProtocol(value) {
    const header = headerValue(value);
    if (!header) return '';
    const protocols = header.split(',').map((protocol) => protocol.trim());
    if (!protocols.length || protocols.some((protocol) =>
        !protocol || !/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(protocol))) {
        return null;
    }
    return protocols[0];
}

function abortConnection(socket, code, message) {
    if (!socket || socket.destroyed) return;
    const body = String(message);
    const response = `HTTP/1.1 ${code} ${body}\r\n` +
        'Connection: close\r\n' +
        'Content-Type: text/plain; charset=utf-8\r\n' +
        `Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`;
    socket.end(response);
}

class WebSocket extends EventEmitter {
    constructor(external, socket, server, compressEnabled, compressThreshold, protocol, extensions) {
        super();
        this.external = external;
        this.CONNECTING = eiows.CONNECTING;
        this.OPEN = eiows.OPEN;
        this.CLOSING = eiows.CLOSING;
        this.CLOSED = eiows.CLOSED;
        this.readyState = eiows.OPEN;
        this.compressEnabled = compressEnabled;
        this.compressThreshold = compressThreshold;
        this.protocol = protocol;
        this.extensions = extensions;
        this.binaryType = 'nodebuffer';

        this._transportSocket = socket;
        this._server = server;
        this._closeCode = 1006;
        this._closeReason = '';
        this._closeTimer = null;
        this._closed = false;
        this._socketError = null;

        // Engine.IO can attach frames pre-encoded with ws.Sender.frame().
        // Writing the header and payload directly avoids native re-framing and
        // another payload copy for broadcasts that use this optimization.
        this._sender = {
            sendFrame: (list, callback) => this._sendFrameList(list, callback)
        };

        socket.on('data', (data) => this._consume(data));
        socket.once('error', (error) => this._onSocketError(error));
        socket.once('close', () => this._finalizeClose());
        socket.once('end', () => {
            if (this.readyState === eiows.OPEN) {
                this.readyState = eiows.CLOSING;
            }
            // Complete our half of the stream when the peer disappears
            // without a WebSocket close frame; otherwise upgraded HTTP
            // sockets may remain half-open until the close timeout.
            if (!socket.destroyed) socket.end();
        });
    }

    get _socket() {
        return this._transportSocket;
    }

    get bufferedAmount() {
        return this._transportSocket && this._transportSocket.writableLength || 0;
    }

    _consume(data) {
        if (!this.external || this.readyState === eiows.CLOSED) return;
        let events;
        try {
            events = native.consume(this.external, data);
        } catch (error) {
            this._fail(error);
            return;
        }

        for (const event of events) {
            if (this.readyState === eiows.CLOSED) break;
            if (event[0] === 0) {
                if (this.readyState === eiows.OPEN) {
                    this.emit('message', event[1], event[2]);
                }
            } else if (event[0] === 1) {
                this._writeFrame(event[1]);
            } else if (event[0] === 2) {
                this._closeCode = event[1];
                this._closeReason = event[2];
                this.readyState = eiows.CLOSING;
                this._startCloseTimeout();
                this._transportSocket.end();
            }
        }
    }

    _writeFrame(frame, callback) {
        const socket = this._transportSocket;
        if (!socket || socket.destroyed || !socket.writable) {
            const error = new Error('socket is not writable');
            if (callback) process.nextTick(callback, error);
            return false;
        }
        try {
            return socket.write(frame, (error) => {
                if (callback) callback(error || undefined);
            });
        } catch (error) {
            if (callback) process.nextTick(callback, error);
            this._fail(error);
            return false;
        }
    }

    _sendFrameList(list, callback) {
        const socket = this._transportSocket;
        if (this.readyState !== eiows.OPEN ||
            !socket || socket.destroyed || !socket.writable) {
            if (callback) process.nextTick(callback, new Error('socket is not writable'));
            return;
        }
        if (!Array.isArray(list) || !list.length ||
            list.some((part) => typeof part !== 'string' && !ArrayBuffer.isView(part))) {
            if (callback) process.nextTick(callback, new TypeError('invalid pre-encoded frame'));
            return;
        }

        let corked = false;
        try {
            if (list.length > 1 && typeof socket.cork === 'function') {
                socket.cork();
                corked = true;
            }
            for (let index = 0; index < list.length - 1; index++) {
                socket.write(list[index]);
            }
            socket.write(list[list.length - 1], (error) => {
                if (callback) callback(error || undefined);
            });
        } catch (error) {
            if (callback) process.nextTick(callback, error);
            this._fail(error);
        } finally {
            if (corked) socket.uncork();
        }
    }

    _onSocketError(error) {
        this._socketError = error;
        this.emit('error', error);
    }

    _fail(error) {
        if (this.readyState === eiows.CLOSED) return;
        this.readyState = eiows.CLOSING;
        if (!this._socketError) {
            this._socketError = error;
            this.emit('error', error);
        }
        if (this._transportSocket && !this._transportSocket.destroyed) {
            this._transportSocket.destroy();
        } else {
            this._finalizeClose();
        }
    }

    _startCloseTimeout() {
        if (this._closeTimer) return;
        this._closeTimer = setTimeout(() => {
            if (this._transportSocket && !this._transportSocket.destroyed) {
                this._transportSocket.destroy();
            }
        }, CLOSE_TIMEOUT);
        if (typeof this._closeTimer.unref === 'function') this._closeTimer.unref();
    }

    _finalizeClose() {
        if (this._closed) return;
        this._closed = true;
        this.readyState = eiows.CLOSED;
        if (this._closeTimer) {
            clearTimeout(this._closeTimer);
            this._closeTimer = null;
        }
        if (this.external) {
            native.dispose(this.external);
            this.external = null;
        }
        const server = this._server;
        this._server = null;
        if (server) server._remove(this);
        this.emit('close', this._closeCode, this._closeReason);
    }

    send(message, options, callback) {
        if (typeof options === 'function') {
            callback = options;
            options = undefined;
        }
        if (this.readyState !== eiows.OPEN || !this.external) {
            if (callback) process.nextTick(callback, new Error('WebSocket is not open'));
            return;
        }

        const binary = typeof message !== 'string';
        const compress = this.compressEnabled &&
            (!options || options.compress !== false) &&
            getMessageByteLength(message, binary) >= this.compressThreshold;

        // Server frames are not masked. For the common uncompressed path,
        // write the small header and original payload as a corked vector so
        // the payload does not need to cross into, and be copied by, native code.
        if (!compress) {
            const payload = binary ? toBuffer(message) : message;
            const header = createFrameHeader(
                getMessageByteLength(payload, binary),
                binary ? eiows.OPCODE_BINARY : eiows.OPCODE_TEXT
            );
            this._sendFrameList(payload.length === 0 ? [header] : [header, payload], callback);
            return;
        }

        let frame;
        try {
            frame = native.frame(
                this.external,
                message,
                binary ? eiows.OPCODE_BINARY : eiows.OPCODE_TEXT,
                compress
            );
        } catch (error) {
            if (callback) process.nextTick(callback, error);
            else this._fail(error);
            return;
        }
        this._writeFrame(frame, callback);
    }

    close(code, data) {
        if (this.readyState === eiows.CLOSING || this.readyState === eiows.CLOSED) return;

        if (code === undefined) {
            if (data !== undefined && toBuffer(data).length) {
                throw new TypeError('a close reason requires a close code');
            }
            code = 0;
        } else if (!Number.isInteger(code) || !isValidCloseCode(code)) {
            throw new RangeError(`invalid WebSocket close code: ${code}`);
        }

        const reason = data === undefined ? Buffer.alloc(0) : toBuffer(data);
        if (reason.length > 123) {
            throw new RangeError('WebSocket close reason exceeds 123 bytes');
        }
        if (!isValidUtf8(reason)) {
            throw new TypeError('WebSocket close reason must be valid UTF-8');
        }

        this.readyState = eiows.CLOSING;
        const frame = native.closeFrame(this.external, code, reason);
        this._startCloseTimeout();
        if (frame) {
            this._writeFrame(frame);
        } else if (this._transportSocket && !this._transportSocket.destroyed) {
            this._transportSocket.end();
        }
    }

    terminate() {
        if (this.readyState === eiows.CLOSED) return;
        this.readyState = eiows.CLOSING;
        if (this._transportSocket && !this._transportSocket.destroyed) {
            this._transportSocket.destroy();
        } else {
            this._finalizeClose();
        }
    }
}

class Server extends EventEmitter {
    constructor(options) {
        super();
        if (!options) throw new TypeError('missing options');

        this._compressEnabled = false;
        this._compressThreshold = eiows.compressThreshold;
        this._nativeOptions = 0;
        if (options.perMessageDeflate !== undefined && options.perMessageDeflate !== false) {
            const perMessageDeflateOptions = options.perMessageDeflate === true
                ? {}
                : options.perMessageDeflate;
            this._nativeOptions |= eiows.PERMESSAGE_DEFLATE;
            this._compressEnabled = true;
            if (!isNaN(perMessageDeflateOptions.threshold) &&
                perMessageDeflateOptions.threshold >= 0) {
                this._compressThreshold = perMessageDeflateOptions.threshold;
            }
            if (perMessageDeflateOptions.clientNoContextTakeover !== false) {
                this._nativeOptions |= eiows.CLIENT_NO_CONTEXT_TAKEOVER;
            }
            if (perMessageDeflateOptions.serverNoContextTakeover !== false) {
                this._nativeOptions |= eiows.SERVER_NO_CONTEXT_TAKEOVER;
            } else {
                this._nativeOptions |= eiows.SLIDING_DEFLATE_WINDOW;
            }
        }

        const maxPayload = options.maxPayload === undefined
            ? DEFAULT_PAYLOAD_LIMIT
            : Number(options.maxPayload);
        if (!Number.isInteger(maxPayload) || maxPayload < 0 || maxPayload > 0x7fffffff) {
            throw new RangeError('maxPayload must be an integer between 0 and 2147483647');
        }
        this._maxPayload = maxPayload;
        this._noDelay = options.noDelay === undefined ? true : Boolean(options.noDelay);
        this._clients = new Set();
        this._closing = false;
        this._closed = false;

        // Retained for source compatibility with previous eiows releases.
        this.serverGroup = this;
        this._pendingUpgradeCallbacks = [];
    }

    handleUpgrade(request, socket, upgradeHead, callback) {
        if (this._closing || this._closed) {
            abortConnection(socket, 503, 'Service Unavailable');
            return;
        }

        const secKey = request.headers['sec-websocket-key'];
        const connectionHeader = request.headers.connection;
        const upgradeHeader = headerValue(request.headers.upgrade);
        const version = headerValue(request.headers['sec-websocket-version']);
        const validUpgrade = socket &&
            typeof socket.write === 'function' &&
            typeof callback === 'function' &&
            request.method === 'GET' &&
            isValidSecWebSocketKey(secKey) &&
            version === '13' &&
            upgradeHeader.toLowerCase() === 'websocket' &&
            hasHeaderToken(connectionHeader, 'upgrade');

        if (!validUpgrade) {
            abortConnection(socket, 400, 'Bad Request');
            return;
        }

        const protocol = selectProtocol(request.headers['sec-websocket-protocol']);
        if (protocol === null) {
            abortConnection(socket, 400, 'Bad Request');
            return;
        }

        if (typeof socket.pause === 'function') socket.pause();
        if (typeof socket.setNoDelay === 'function') socket.setNoDelay(this._noDelay);

        let external;
        let extensions;
        try {
            [external, extensions] = native.createSession(
                this._nativeOptions,
                this._maxPayload,
                headerValue(request.headers['sec-websocket-extensions'])
            );
        } catch (error) {
            abortConnection(socket, 500, 'Internal Server Error');
            this.emit('error', error);
            return;
        }

        const accept = createHash('sha1')
            .update(secKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
            .digest('base64');
        const headers = [
            'HTTP/1.1 101 Switching Protocols',
            'Upgrade: websocket',
            'Connection: Upgrade',
            `Sec-WebSocket-Accept: ${accept}`
        ];
        if (extensions) headers.push(`Sec-WebSocket-Extensions: ${extensions}`);
        if (protocol) headers.push(`Sec-WebSocket-Protocol: ${protocol}`);

        try {
            this.emit('headers', headers, request);
            if (headers.some((header) => typeof header !== 'string' || /[\r\n]/.test(header))) {
                throw new TypeError('invalid WebSocket response header');
            }
        } catch (error) {
            native.dispose(external);
            abortConnection(socket, 500, 'Internal Server Error');
            this.emit('error', error);
            return;
        }

        const webSocket = new WebSocket(
            external,
            socket,
            this,
            this._compressEnabled,
            this._compressThreshold,
            protocol,
            extensions
        );
        this._clients.add(webSocket);

        try {
            socket.write(headers.join('\r\n') + '\r\n\r\n');
            callback(webSocket, request);
            if (upgradeHead && upgradeHead.length) webSocket._consume(upgradeHead);
            if (!socket.destroyed && typeof socket.resume === 'function') socket.resume();
        } catch (error) {
            webSocket.terminate();
            throw error;
        }
    }

    _remove(webSocket) {
        this._clients.delete(webSocket);
        if (this._closing && !this._clients.size) this._finishClose();
    }

    _finishClose() {
        if (this._closed) return;
        this._closed = true;
        this.serverGroup = null;
        this.emit('close');
    }

    close(callback) {
        if (typeof callback === 'function') {
            if (this._closed) process.nextTick(callback);
            else this.once('close', callback);
        }
        if (this._closing || this._closed) return;
        this._closing = true;
        for (const webSocket of Array.from(this._clients)) {
            webSocket.close(1001);
        }
        if (!this._clients.size) process.nextTick(() => this._finishClose());
    }
}

eiows.WebSocket = WebSocket;
eiows.Server = Server;
eiows.native = native;

module.exports = eiows;
