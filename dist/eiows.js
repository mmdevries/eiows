'use strict';

const { createHash } = require('node:crypto');
const { EventEmitter } = require('node:events');

const DEFAULT_PAYLOAD_LIMIT = 16777216;
const DEFAULT_BACKPRESSURE_LIMIT = 67108864;
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
    const binary = `./eiows_${process.versions.modules}.node`;
    try {
        return require(binary);
    } catch (error) {
        throw new Error(error.toString() + '\n\nNo compatible eiows native binary was found. ' +
            `Expected ${binary}. Please install a supported C++20 compiler and ` +
            'rebuild the module from source for this exact Node.js release.');
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

function createFrameHeader(payloadLength, opCode) {
    const cache = opCode === eiows.OPCODE_TEXT
        ? createFrameHeader.textCache
        : createFrameHeader.binaryCache;
    if (cache.length === payloadLength) return cache.header;

    let header;
    if (payloadLength < 126) {
        header = Buffer.allocUnsafe(2);
        header[0] = 0x80 | opCode;
        header[1] = payloadLength;
    } else if (payloadLength <= 0xffff) {
        header = Buffer.allocUnsafe(4);
        header[0] = 0x80 | opCode;
        header[1] = 126;
        header.writeUInt16BE(payloadLength, 2);
    } else {
        header = Buffer.allocUnsafe(10);
        header[0] = 0x80 | opCode;
        header[1] = 127;
        header.writeBigUInt64BE(BigInt(payloadLength), 2);
    }
    cache.length = payloadLength;
    cache.header = header;
    return header;
}

createFrameHeader.textCache = { length: -1, header: null };
createFrameHeader.binaryCache = { length: -1, header: null };

// Engine.IO treats wsPreEncodedFrame arrays as immutable and reuses them for
// every recipient of a broadcast. Coalescing a small header/payload pair once
// keeps native TLS to one SSL_write per connection.
const nativeFrameCache = new WeakMap();
const MAX_CACHED_NATIVE_FRAME_BYTES = 1038;

function prepareNativeFrame(list) {
    if (!Array.isArray(list) || !list.length) return null;

    const cached = nativeFrameCache.get(list);
    if (cached) return cached;

    let bytes = 0;
    for (const part of list) {
        if (typeof part !== 'string' && !ArrayBuffer.isView(part)) return null;
        bytes += Buffer.byteLength(part);
        if (bytes > MAX_CACHED_NATIVE_FRAME_BYTES) return list;
    }

    if (list.length === 1) return list;
    const frame = Buffer.concat(list.map(toBuffer), bytes);
    nativeFrameCache.set(list, frame);
    return frame;
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

function isHttp11OrHigher(request) {
    if (!request || !Number.isInteger(request.httpVersionMajor) ||
        !Number.isInteger(request.httpVersionMinor)) {
        return false;
    }
    return request.httpVersionMajor > 1 ||
        (request.httpVersionMajor === 1 && request.httpVersionMinor >= 1);
}

function isValidHostHeader(request) {
    if (!request || !request.headers) return false;
    const host = request.headers.host;
    if (typeof host !== 'string' || !host ||
        !/^[\x21-\x7e]+$/.test(host) || /[\\/@?#,]/.test(host)) {
        return false;
    }

    // IncomingMessage.headers cannot represent duplicate Host fields. Consult
    // rawHeaders as well so an invalid request cannot be normalized into a
    // valid-looking one by Node's HTTP parser.
    if (Array.isArray(request.rawHeaders)) {
        let hostFields = 0;
        for (let index = 0; index < request.rawHeaders.length; index += 2) {
            if (String(request.rawHeaders[index]).toLowerCase() === 'host') hostFields++;
        }
        if (hostFields !== 1) return false;
    }

    try {
        const parsed = new URL(`http://${host}/`);
        return Boolean(parsed.hostname) && !parsed.username && !parsed.password &&
            parsed.pathname === '/' && !parsed.search && !parsed.hash;
    } catch {
        return false;
    }
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
    const seen = new Set();
    for (const protocol of protocols) {
        if (!protocol || !/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(protocol) ||
            seen.has(protocol)) {
            return null;
        }
        seen.add(protocol);
    }
    return protocols[0];
}

function abortConnection(socket, code, message, headers = []) {
    if (!socket || socket.destroyed) return;
    const body = String(message);
    const response = `HTTP/1.1 ${code} ${body}\r\n` +
        'Connection: close\r\n' +
        headers.map((header) => `${header}\r\n`).join('') +
        'Content-Type: text/plain; charset=utf-8\r\n' +
        `Content-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`;
    socket.once('finish', () => socket.destroy());
    socket.end(response);
}

class WebSocket extends EventEmitter {
    constructor(external, socket, server, compressEnabled, compressThreshold, protocol, extensions,
                textAsBuffer, maxBackpressure) {
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
        this._textAsBuffer = textAsBuffer;
        this._maxBackpressure = maxBackpressure;

        this._transportSocket = socket;
        this._server = server;
        this._closeCode = 1006;
        this._closeReason = '';
        this._closeTimer = null;
        this._closed = false;
        this._socketError = null;
        this._nativeTransport = false;
        this._nativeActive = false;
        this._upgradePending = false;
        this._socketInfo = {
            remoteAddress: socket.remoteAddress,
            remotePort: socket.remotePort,
            remoteFamily: socket.remoteFamily
        };

        // Engine.IO can attach frames pre-encoded with ws.Sender.frame().
        // Writing the header and payload directly avoids native re-framing and
        // another payload copy for broadcasts that use this optimization.
        this._sender = {
            sendFrame: (list, callback) => this._sendFrameList(list, callback)
        };

        if (socket._handle) {
            this._nativeTransport = native.attachTransport(
                external,
                socket._handle,
                this,
                textAsBuffer,
                Boolean(socket.encrypted),
                maxBackpressure
            );
        }
        if (!this._nativeTransport) {
            socket.once('error', (error) => this._onSocketError(error));
            socket.once('close', () => this._finalizeClose());
            socket.once('end', () => {
                if (this.readyState === eiows.OPEN) {
                    this.readyState = eiows.CLOSING;
                }
                if (!socket.destroyed) socket.end();
            });
            socket.on('data', (data) => this._consume(data));
        }
    }

    get _socket() {
        return this._transportSocket || this._socketInfo;
    }

    get bufferedAmount() {
        if (this._nativeTransport && this.external) {
            return native.transportBufferedAmount(this.external);
        }
        return this._transportSocket && this._transportSocket.writableLength || 0;
    }

    _completeNativeUpgrade(response, upgradeHead, callback, request) {
        const socket = this._transportSocket;
        let transferError = null;
        const onError = (error) => {
            transferError = error;
        };
        socket.once('error', onError);
        socket.once('close', () => {
            socket.removeListener('error', onError);
            this._transportSocket = null;
            if (!this._upgradePending || this.readyState !== eiows.CONNECTING) {
                this._upgradePending = false;
                this._finalizeClose();
                return;
            }
            const activate = () => {
                if (!this.external || !this._upgradePending ||
                    this.readyState !== eiows.CONNECTING) {
                    this._upgradePending = false;
                    this._finalizeClose();
                    return;
                }
                try {
                    const status = native.activateTransport(this.external);
                    if (status < 0) {
                        throw new Error(`native socket ownership activation failed (${status})`);
                    }
                    this._nativeActive = true;
                    if (transferError) throw transferError;
                    if (!this._writeFrame(response)) return;
                    this._upgradePending = false;
                    this.readyState = eiows.OPEN;
                    callback(this, request);
                    if (upgradeHead && upgradeHead.length) this._consume(upgradeHead);
                } catch (error) {
                    this._fail(error);
                }
            };
            if (socket.encrypted) {
                // Node schedules TLSWrap.destroySSL() from its close listener.
                // Activate one turn later so the retained SSL/BIO state has a
                // single active driver throughout the ownership transition.
                setImmediate(activate);
            } else {
                activate();
            }
        });
        setImmediate(() => {
            if (this._upgradePending && this.readyState === eiows.CONNECTING &&
                this._transportSocket === socket) {
                socket.destroy();
            }
        });
    }

    _consume(data) {
        if (!this.external || this.readyState === eiows.CLOSED) return;
        if (this._nativeTransport) {
            try {
                native.feedTransport(this.external, data);
            } catch (error) {
                this._fail(error);
            }
            return;
        }
        let events;
        try {
            events = native.consume(this.external, data, this._textAsBuffer);
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

    _onNativeMessage(message, isBinary) {
        if (this.readyState === eiows.OPEN) this.emit('message', message, isBinary);
    }

    _onNativeClose(code, reason) {
        if (this.readyState === eiows.CLOSED) return;
        this._closeCode = code;
        this._closeReason = reason;
        this.readyState = eiows.CLOSING;
        this._startCloseTimeout();
    }

    _onNativeClosed() {
        this._finalizeClose();
    }

    _onNativeBackpressure() {
        if (this.readyState !== eiows.CLOSED) this.readyState = eiows.CLOSING;
    }

    _onNativeTransportError(message) {
        if (this.readyState === eiows.CLOSED) return;
        this.readyState = eiows.CLOSING;
        const error = new Error(`native socket transport failed: ${message}`);
        if (!this._socketError) {
            this._socketError = error;
            this.emit('error', error);
        }
    }

    _finishNativeWrite(status, callback) {
        if (status >= 0) {
            if (status === 0 && callback) process.nextTick(callback);
            return true;
        }
        const error = new Error(`native socket write failed (${status})`);
        if (callback) process.nextTick(callback, error);
        this._fail(error);
        return false;
    }

    _writeFrame(frame, callback) {
        const socket = this._transportSocket;
        if (this._nativeTransport) {
            if (!this._nativeActive || !this.external) {
                const error = new Error('native socket transport is not active');
                if (callback) process.nextTick(callback, error);
                return false;
            }
            try {
                return this._finishNativeWrite(
                    native.writeTransportFrames(this.external, [frame], callback),
                    callback
                );
            } catch (error) {
                if (callback) process.nextTick(callback, error);
                this._fail(error);
                return false;
            }
        }
        if (!socket || socket.destroyed || !socket.writable) {
            const error = new Error('socket is not writable');
            if (callback) process.nextTick(callback, error);
            return false;
        }
        if (!this._acceptFallbackWrite(Buffer.byteLength(frame), callback)) return false;
        try {
            if (callback) {
                return socket.write(frame, (error) => callback(error || undefined));
            }
            return socket.write(frame);
        } catch (error) {
            if (callback) process.nextTick(callback, error);
            this._fail(error);
            return false;
        }
    }

    _sendFrameList(list, callback) {
        const socket = this._transportSocket;
        if (this.readyState !== eiows.OPEN || !this.external ||
            (!this._nativeTransport && (!socket || socket.destroyed || !socket.writable))) {
            if (callback) process.nextTick(callback, new Error('socket is not writable'));
            return;
        }
        const nativeFrame = this._nativeTransport ? prepareNativeFrame(list) : list;
        if (!nativeFrame || (!this._nativeTransport &&
            (!Array.isArray(list) || !list.length ||
                list.some((part) => typeof part !== 'string' && !ArrayBuffer.isView(part))))) {
            if (callback) process.nextTick(callback, new TypeError('invalid pre-encoded frame'));
            return;
        }

        if (this._nativeTransport) {
            if (!this._nativeActive) {
                if (callback) process.nextTick(
                    callback, new Error('native socket transport is not active'));
                return;
            }
            try {
                this._finishNativeWrite(
                    (Buffer.isBuffer(nativeFrame)
                        ? native.writeTransportFrame(this.external, nativeFrame, callback)
                        : native.writeTransportFrames(this.external, nativeFrame, callback)),
                    callback
                );
            } catch (error) {
                if (callback) process.nextTick(callback, error);
                this._fail(error);
            }
            return;
        }

        if (list.length === 1) {
            this._writeFrame(list[0], callback);
            return;
        }
        this._writeFrameParts(list, callback);
    }

    _writeFrameParts(list, callback) {
        const socket = this._transportSocket;
        const bytes = list.reduce((total, part) => total + Buffer.byteLength(part), 0);
        if (!this._acceptFallbackWrite(bytes, callback)) return;
        let corked = false;
        try {
            if (typeof socket.cork === 'function') {
                socket.cork();
                corked = true;
            }
            for (let index = 0; index < list.length - 1; index++) {
                socket.write(list[index]);
            }
            if (callback) {
                socket.write(list[list.length - 1], (error) =>
                    callback(error || undefined));
            } else {
                socket.write(list[list.length - 1]);
            }
        } catch (error) {
            if (callback) process.nextTick(callback, error);
            this._fail(error);
        } finally {
            if (corked) socket.uncork();
        }
    }

    _writeTwoFrameParts(header, payload, callback) {
        const socket = this._transportSocket;
        if (!this._acceptFallbackWrite(header.length + payload.length, callback)) return;
        let corked = false;
        try {
            if (typeof socket.cork === 'function') {
                socket.cork();
                corked = true;
            }
            socket.write(header);
            if (callback) {
                socket.write(payload, (error) => callback(error || undefined));
            } else {
                socket.write(payload);
            }
        } catch (error) {
            if (callback) process.nextTick(callback, error);
            this._fail(error);
        } finally {
            if (corked) socket.uncork();
        }
    }

    _acceptFallbackWrite(bytes, callback) {
        const socket = this._transportSocket;
        if (!this._maxBackpressure || !socket ||
            socket.writableLength + bytes <= this._maxBackpressure) {
            return true;
        }
        const error = new Error(
            `maximum backpressure of ${this._maxBackpressure} bytes exceeded`
        );
        error.code = 'EIOWS_MAX_BACKPRESSURE';
        if (callback) process.nextTick(callback, error);
        this.terminate();
        return false;
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
        if (this._upgradePending) {
            this._abortPendingUpgrade(false);
            return;
        }
        if (this._nativeTransport && this.external) {
            native.terminateTransport(this.external);
            if (!this._nativeActive) this._finalizeClose();
        } else if (this._transportSocket && !this._transportSocket.destroyed) {
            this._transportSocket.destroy();
        } else {
            this._finalizeClose();
        }
    }

    _abortPendingUpgrade(sendResponse) {
        if (!this._upgradePending) return false;
        this.readyState = eiows.CLOSING;
        if (this._nativeTransport && this.external) {
            native.terminateTransport(this.external);
        }

        const socket = this._transportSocket;
        if (socket) {
            if (!socket.destroyed) {
                if (sendResponse) abortConnection(socket, 503, 'Service Unavailable');
                else socket.destroy();
            }
            // Both native and fallback upgrade paths finalize from the socket's
            // close listener. Keeping the session registered until then makes
            // Server.close() wait for the transport to be fully released.
            return true;
        }

        this._upgradePending = false;
        if (this._nativeActive) return true;
        this._finalizeClose();
        return true;
    }

    _startCloseTimeout() {
        if (this._closeTimer) return;
        this._closeTimer = setTimeout(() => {
            if (this._nativeTransport && this.external) {
                native.terminateTransport(this.external);
            } else if (this._transportSocket && !this._transportSocket.destroyed) {
                this._transportSocket.destroy();
            }
        }, CLOSE_TIMEOUT);
        if (typeof this._closeTimer.unref === 'function') this._closeTimer.unref();
    }

    _finalizeClose() {
        if (this._closed) return;
        this._closed = true;
        this._upgradePending = false;
        this.readyState = eiows.CLOSED;
        if (this._closeTimer) {
            clearTimeout(this._closeTimer);
            this._closeTimer = null;
        }
        if (this.external) {
            native.dispose(this.external);
            this.external = null;
        }
        this._nativeActive = false;
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

        if (this._nativeTransport) {
            try {
                // Engine.IO's parser normalizes outbound packets to strings,
                // ArrayBuffers or views. Native code consumes those values directly;
                // only inspect their length when compression can actually run.
                const compress = this.compressEnabled &&
                    (!options || options.compress !== false) &&
                    (binary ? message.byteLength : Buffer.byteLength(message)) >=
                        this.compressThreshold;
                this._finishNativeWrite(
                    native.writeTransportMessage(
                        this.external,
                        message,
                        binary ? eiows.OPCODE_BINARY : eiows.OPCODE_TEXT,
                        compress,
                        callback
                    ),
                    callback
                );
            } catch (error) {
                if (callback) process.nextTick(callback, error);
                else this._fail(error);
            }
            return;
        }

        const payload = binary ? toBuffer(message) : message;
        const payloadLength = binary ? payload.length : Buffer.byteLength(payload);
        const compress = this.compressEnabled &&
            (!options || options.compress !== false) &&
            payloadLength >= this.compressThreshold;

        // Server frames are not masked. For the common uncompressed path,
        // write the small header and original payload as a corked vector so
        // the payload does not need to cross into, and be copied by, native code.
        if (!compress) {
            const header = createFrameHeader(
                payloadLength,
                binary ? eiows.OPCODE_BINARY : eiows.OPCODE_TEXT
            );
            if (payloadLength === 0) {
                this._writeFrame(header, callback);
            } else {
                this._writeTwoFrameParts(header, payload, callback);
            }
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
        if (this._upgradePending) {
            this._abortPendingUpgrade(true);
            return;
        }

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
        this._startCloseTimeout();
        if (this._nativeTransport) {
            try {
                const status = native.writeTransportClose(this.external, code, reason);
                if (status < 0) native.terminateTransport(this.external);
            } catch (error) {
                this._fail(error);
            }
        } else {
            const frame = native.closeFrame(this.external, code, reason);
            if (frame) {
                this._writeFrame(frame);
            } else if (this._transportSocket && !this._transportSocket.destroyed) {
                this._transportSocket.end();
            }
        }
    }

    terminate() {
        if (this.readyState === eiows.CLOSED) return;
        if (this._upgradePending) {
            this._abortPendingUpgrade(false);
            return;
        }
        this.readyState = eiows.CLOSING;
        if (this._nativeTransport && this.external) {
            native.terminateTransport(this.external);
            if (!this._nativeActive) this._finalizeClose();
        } else if (this._transportSocket && !this._transportSocket.destroyed) {
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
        const maxBackpressure = options.maxBackpressure === undefined
            ? DEFAULT_BACKPRESSURE_LIMIT
            : Number(options.maxBackpressure);
        if (!Number.isInteger(maxBackpressure) || maxBackpressure < 0 ||
            maxBackpressure > 0x7fffffff) {
            throw new RangeError(
                'maxBackpressure must be an integer between 0 and 2147483647'
            );
        }
        this._maxBackpressure = maxBackpressure;
        this._noDelay = options.noDelay === undefined ? true : Boolean(options.noDelay);
        // Match ws: text is delivered as Buffer with isBinary === false.
        // Engine.IO performs its normal string conversion in the transport.
        this._textAsBuffer = options.textAsString !== true;
        this._clients = new Set();
        this._closing = false;
        this._closed = false;
        this._compressionContext = this._compressEnabled
            ? native.createCompressionContext()
            : null;

        // Retained for source compatibility with previous eiows releases.
        this.serverGroup = this;
        this._pendingUpgradeCallbacks = [];
    }

    handleUpgrade(request, socket, upgradeHead, callback) {
        if (this._closing || this._closed) {
            abortConnection(socket, 503, 'Service Unavailable');
            return;
        }

        const requestHeaders = request && request.headers;
        const secKey = requestHeaders && requestHeaders['sec-websocket-key'];
        const connectionHeader = requestHeaders && requestHeaders.connection;
        const upgradeHeader = headerValue(requestHeaders && requestHeaders.upgrade);
        const version = headerValue(requestHeaders && requestHeaders['sec-websocket-version']);
        const validUpgrade = request && requestHeaders && socket &&
            typeof socket.write === 'function' &&
            typeof callback === 'function' &&
            request.method === 'GET' &&
            isHttp11OrHigher(request) &&
            isValidHostHeader(request) &&
            isValidSecWebSocketKey(secKey) &&
            upgradeHeader.toLowerCase() === 'websocket' &&
            hasHeaderToken(connectionHeader, 'upgrade');

        if (!validUpgrade) {
            abortConnection(socket, 400, 'Bad Request');
            return;
        }

        if (version !== '13') {
            if (version) {
                abortConnection(
                    socket,
                    426,
                    'Upgrade Required',
                    ['Sec-WebSocket-Version: 13']
                );
            } else {
                abortConnection(socket, 400, 'Bad Request');
            }
            return;
        }

        const protocol = selectProtocol(requestHeaders['sec-websocket-protocol']);
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
                headerValue(requestHeaders['sec-websocket-extensions']),
                this._compressionContext
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

        // `headers` is intentionally re-entrant. A listener may initiate
        // shutdown, in which case this request must not create a client after
        // the server has committed to closing.
        if (this._closing || this._closed) {
            native.dispose(external);
            abortConnection(socket, 503, 'Service Unavailable');
            return;
        }

        const webSocket = new WebSocket(
            external,
            socket,
            this,
            this._compressEnabled,
            this._compressThreshold,
            protocol,
            extensions,
            this._textAsBuffer,
            this._maxBackpressure
        );
        webSocket._upgradePending = true;
        webSocket.readyState = eiows.CONNECTING;
        this._clients.add(webSocket);

        try {
            const response = headers.join('\r\n') + '\r\n\r\n';
            if (webSocket._nativeTransport) {
                webSocket._completeNativeUpgrade(response, upgradeHead, callback, request);
            } else {
                socket.write(response);
                if (!webSocket._upgradePending ||
                    webSocket.readyState !== eiows.CONNECTING) {
                    return;
                }
                webSocket._upgradePending = false;
                webSocket.readyState = eiows.OPEN;
                callback(webSocket, request);
                if (upgradeHead && upgradeHead.length) webSocket._consume(upgradeHead);
                if (!socket.destroyed && typeof socket.resume === 'function') socket.resume();
            }
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
        this._compressionContext = null;
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

module.exports = eiows;
