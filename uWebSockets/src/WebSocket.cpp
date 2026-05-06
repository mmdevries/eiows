#include "WebSocket.h"
#include "Hub.h"
#include <utility>
#include <algorithm>
#include <string>

namespace eioWS {
    static const size_t FRAGMENT_BUFFER_RETAIN_LIMIT = 1024 * 1024;

    static inline void trimFragmentBuffer(std::string &buffer) {
        if (buffer.empty() && buffer.capacity() > FRAGMENT_BUFFER_RETAIN_LIMIT) {
            std::string().swap(buffer);
        }
    }

    const WebSocketProtocolHooks WebSocket::protocolHooks = {
        WebSocket::refusePayloadLength,
        WebSocket::setCompressed,
        WebSocket::forceClose,
        WebSocket::handleFragment
    };

    WebSocket::WebSocket(unsigned int maxP, bool perMessageDeflate, uS::Socket *socket) :
        uS::Socket(std::move(*socket)) {
        maxPayload = maxP;
        compressionStatus = perMessageDeflate ? CompressionStatus::ENABLED : CompressionStatus::DISABLED;

        // if we are created in a group with sliding deflate window allocate it here
        if (Group::from(this)->extensionOptions & SLIDING_DEFLATE_WINDOW) {
            slidingDeflateWindow = Hub::allocateDefaultCompressor(new z_stream{});
        }
    }

    size_t WebSocket::transformMessage(const char *src, char *dst, size_t length, void *transformData) {
        TransformData *data = static_cast<TransformData *>(transformData);
        if (data->compress) {
            char *deflated = Group::from(data->webSocket)->hub->deflate(const_cast<char *>(src), length, reinterpret_cast<z_stream *>(data->webSocket->slidingDeflateWindow));
            return WebSocketProtocol::formatMessage(dst, deflated, length, data->opCode, length, true);
        }

        return WebSocketProtocol::formatMessage(dst, src, length, data->opCode, length, false);
    }

    size_t WebSocket::transformPreparedMessage(char *dst, size_t length, void *transformData) {
        PreparedTransformData *data = static_cast<PreparedTransformData *>(transformData);
        size_t headerLength = WebSocketProtocol::formatMessageHeader(dst, data->opCode, length, false);
        return headerLength + data->writePayload(dst + headerLength, length, data->data);
    }

    void WebSocket::deleteSocket(uS::Poll *p) {
        delete static_cast<WebSocket *>(p);
    }

    /*
     * Frames and sends a WebSocket message.
     *
     * Hints: Consider using any of the prepare function if any of their
     * use cases match what you are trying to achieve (pub/sub, broadcast)
     *
     */
    void WebSocket::send(const char *message, size_t length, OpCode opCode, void(*callback)(WebSocket *webSocket, void *data, bool cancelled, void *reserved), void *callbackData, bool compress) {
        TransformData transformData = {opCode, compress && compressionStatus == WebSocket::CompressionStatus::ENABLED && opCode < 3, this};
        sendTransformed(message, length, transformMessage, &transformData, (void(*)(void *, void *, bool, void *)) callback, callbackData);
    }

    void WebSocket::sendPrepared(size_t length, OpCode opCode, size_t (*writePayload)(char *dst, size_t length, void *data), void *data, void(*callback)(WebSocket *webSocket, void *data, bool cancelled, void *reserved), void *callbackData) {
        PreparedTransformData transformData = {opCode, writePayload, data};
        uS::Socket::sendPrepared(length, transformPreparedMessage, &transformData, (void(*)(void *, void *, bool, void *)) callback, callbackData);
    }

    uS::Socket *WebSocket::onData(uS::Socket *s, char *data, size_t length) {
        WebSocket *webSocket = static_cast<WebSocket *>(s);
        if (!webSocket->isShuttingDown()) {
            WebSocketProtocol::consume(data, (unsigned int) length, webSocket, protocolHooks);
        }

        return webSocket;
    }

    /*
     * Immediately terminates this WebSocket. Will call onDisconnection of its Group.
     *
     * Hints: Close code will be 1006 and message will be empty.
     *
     */
    void WebSocket::terminate() {
        WebSocket::onEnd(this);
    }

    /*
     * Immediately calls onDisconnection of its Group and begins a passive
     * WebSocket closedown handshake in the background (might succeed or not,
     * we don't care).
     *
     * Hints: Close code and message will be what you pass yourself.
     *
     */

    void WebSocket::close(int code, const char *message, size_t length) {
        static const int MAX_CLOSE_PAYLOAD = 123;
        if (isShuttingDown()) {
            return;
        }
        setShuttingDown(true);
        length = std::min<size_t>(MAX_CLOSE_PAYLOAD, length);
        Group::from(this)->removeWebSocket(this);
        Group::from(this)->disconnectionHandler(this, code, const_cast<char *>(message), length);


        startTimeout(WebSocket::onEnd);

        char closePayload[MAX_CLOSE_PAYLOAD + 2];
        int closePayloadLength = static_cast<int>(WebSocketProtocol::formatClosePayload(closePayload, code, message, length));
        send(closePayload, closePayloadLength, OpCode::CLOSE, [](WebSocket *p, void *, bool cancelled, void *) {
            if (!cancelled) {
                p->shutdown();
            }
        });
    }

    void WebSocket::onEnd(uS::Socket *s) {
        WebSocket *webSocket = static_cast<WebSocket *>(s);
        Group *group = Group::from(webSocket);
        if (!webSocket->isShuttingDown()) {
            group->removeWebSocket(webSocket);
            group->disconnectionHandler(webSocket, 1006, nullptr, 0);
        } else {
            webSocket->cancelTimeout();
        }

        webSocket->closeSocket(deleteSocket);

        while (!webSocket->messageQueue.empty()) {
            Queue::Message *message = webSocket->messageQueue.front();
            if (message->callback) {
                message->callback(nullptr, message->callbackData, true, nullptr);
            }
            webSocket->freeMessage(webSocket->messageQueue.pop());
        }

        // remove any per-websocket zlib memory
        if (webSocket->slidingDeflateWindow) {
            // this relates to Hub::allocateDefaultCompressor
            deflateEnd(reinterpret_cast<z_stream *>(webSocket->slidingDeflateWindow));
            delete reinterpret_cast<z_stream *>(webSocket->slidingDeflateWindow);
            webSocket->slidingDeflateWindow = nullptr;
        }

        group->onSocketClosed();
    }


    bool WebSocket::handleFragment(char *data, size_t length, unsigned int remainingBytes, int opCode, bool fin, WebSocketState *webSocketState) {
        WebSocket *webSocket = static_cast<WebSocket *>(webSocketState);
        Group *group = Group::from(webSocket);

        if (opCode < 3) {
            if (!remainingBytes && fin && !webSocket->fragmentBuffer.length()) {
                if (webSocket->compressionStatus == WebSocket::CompressionStatus::COMPRESSED_FRAME) {
                    webSocket->compressionStatus = WebSocket::CompressionStatus::ENABLED;
                    data = group->hub->inflate(data, length, group->maxPayload);
                    if (!data) {
                        forceClose(webSocketState);
                        return true;
                    }
                }

                if (opCode == 1 && !WebSocketProtocol::isValidUtf8((unsigned char *) data, length)) {
                    forceClose(webSocketState);
                    return true;
                }

                group->messageHandler(webSocket, data, length, (OpCode) opCode);
                if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                    return true;
                }
            } else {
                size_t requiredCapacity = webSocket->fragmentBuffer.length() + length + remainingBytes + 4;
                if (webSocket->fragmentBuffer.capacity() < requiredCapacity) {
                    webSocket->fragmentBuffer.reserve(requiredCapacity);
                }
                webSocket->fragmentBuffer.append(data, length);
                if (!remainingBytes && fin) {
                    length = webSocket->fragmentBuffer.length();
                    if (webSocket->compressionStatus == WebSocket::CompressionStatus::COMPRESSED_FRAME) {
                        webSocket->compressionStatus = WebSocket::CompressionStatus::ENABLED;
                        webSocket->fragmentBuffer.append("....");
                        data = group->hub->inflate(reinterpret_cast<char *>(webSocket->fragmentBuffer.data()), length, group->maxPayload);
                        if (!data) {
                            forceClose(webSocketState);
                            return true;
                        }
                    } else {
                        data = reinterpret_cast<char *>(webSocket->fragmentBuffer.data());
                    }

                    if (opCode == 1 && !WebSocketProtocol::isValidUtf8((unsigned char *) data, length)) {
                        forceClose(webSocketState);
                        return true;
                    }

                    group->messageHandler(webSocket, data, length, (OpCode) opCode);
                    if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                        return true;
                    }
                    webSocket->fragmentBuffer.clear();
                    trimFragmentBuffer(webSocket->fragmentBuffer);
                }
            }
        } else {
            if (!remainingBytes && fin && !webSocket->controlTipLength) {
                if (opCode == CLOSE) {
                    WebSocketProtocol::CloseFrame closeFrame = WebSocketProtocol::parseClosePayload(data, length);
                    if (closeFrame.code == 1006) {
                        webSocket->close(1002, nullptr, 0);
                        return true;
                    }
                    webSocket->close(closeFrame.code, closeFrame.message, closeFrame.length);
                    return true;
                } else {
                    if (opCode == PING) {
                        webSocket->send(data, length, (OpCode) OpCode::PONG);
                        if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                            return true;
                        }
                    } else if (opCode == PONG) {
                        if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                            return true;
                        }
                    }
                }
            } else {
                size_t requiredCapacity = webSocket->fragmentBuffer.length() + length + remainingBytes;
                if (webSocket->fragmentBuffer.capacity() < requiredCapacity) {
                    webSocket->fragmentBuffer.reserve(requiredCapacity);
                }
                webSocket->fragmentBuffer.append(data, length);
                webSocket->controlTipLength += length;

                if (!remainingBytes && fin) {
                    char *controlBuffer = reinterpret_cast<char *>(webSocket->fragmentBuffer.data()) + webSocket->fragmentBuffer.length() - webSocket->controlTipLength;
                    if (opCode == CLOSE) {
                        WebSocketProtocol::CloseFrame closeFrame = WebSocketProtocol::parseClosePayload(controlBuffer, webSocket->controlTipLength);
                        if (closeFrame.code == 1006) {
                            webSocket->close(1002, nullptr, 0);
                            return true;
                        }
                        webSocket->close(closeFrame.code, closeFrame.message, closeFrame.length);
                        return true;
                    } else {
                        if (opCode == PING) {
                            webSocket->send(controlBuffer, webSocket->controlTipLength, (OpCode) OpCode::PONG);
                            if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                                return true;
                            }
                        } else if (opCode == PONG) {
                            if (webSocket->isClosed() || webSocket->isShuttingDown()) {
                                return true;
                            }
                        }
                    }

                    webSocket->fragmentBuffer.resize(webSocket->fragmentBuffer.length() - webSocket->controlTipLength);
                    webSocket->controlTipLength = 0;
                    trimFragmentBuffer(webSocket->fragmentBuffer);
                }
            }
        }

        return false;
    }

    static void base64(unsigned char *src, char *dst) {
        static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 18; i += 3) {
            *dst++ = b64[(src[i] >> 2) & 63];
            *dst++ = b64[((src[i] & 3) << 4) | ((src[i + 1] & 240) >> 4)];
            *dst++ = b64[((src[i + 1] & 15) << 2) | ((src[i + 2] & 192) >> 6)];
            *dst++ = b64[src[i + 2] & 63];
        }
        *dst++ = b64[(src[18] >> 2) & 63];
        *dst++ = b64[((src[18] & 3) << 4) | ((src[19] & 240) >> 4)];
        *dst++ = b64[((src[19] & 15) << 2)];
        *dst++ = '=';
    }

    void WebSocket::upgrade(const char *secKey, const std::string& extensionsResponse, const char *subprotocol, size_t subprotocolLength) {
        Queue::Message *messagePtr;

        unsigned char shaInput[] = "XXXXXXXXXXXXXXXXXXXXXXXX258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        memcpy(shaInput, secKey, 24);
        unsigned char shaDigest[SHA_DIGEST_LENGTH];
        SHA1(shaInput, sizeof(shaInput) - 1, shaDigest);

        char upgradeBuffer[1024];
        memcpy(upgradeBuffer, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ", 97);
        base64(shaDigest, upgradeBuffer + 97);
        memcpy(upgradeBuffer + 125, "\r\n", 2);
        size_t upgradeResponseLength = 127;

        if (extensionsResponse.length() && extensionsResponse.length() < 200) {
            memcpy(upgradeBuffer + upgradeResponseLength, "Sec-WebSocket-Extensions: ", 26);
            memcpy(upgradeBuffer + upgradeResponseLength + 26, extensionsResponse.data(), extensionsResponse.length());
            memcpy(upgradeBuffer + upgradeResponseLength + 26 + extensionsResponse.length(), "\r\n", 2);
            upgradeResponseLength += 26 + extensionsResponse.length() + 2;
        }
        // select first protocol
        for (unsigned int i = 0; i < subprotocolLength; i++) {
            if (subprotocol[i] == ',') {
                subprotocolLength = i;
                break;
            }
        }
        if (subprotocolLength && subprotocolLength < 200) {
            memcpy(upgradeBuffer + upgradeResponseLength, "Sec-WebSocket-Protocol: ", 24);
            memcpy(upgradeBuffer + upgradeResponseLength + 24, subprotocol, subprotocolLength);
            memcpy(upgradeBuffer + upgradeResponseLength + 24 + subprotocolLength, "\r\n", 2);
            upgradeResponseLength += 24 + subprotocolLength + 2;
        }
        memcpy(upgradeBuffer + upgradeResponseLength, "\r\n", 2);
        upgradeResponseLength += 2;

        messagePtr = allocMessage(upgradeResponseLength, upgradeBuffer);

        bool waiting;
        if (write(messagePtr, waiting)) {
            if (!waiting) {
                freeMessage(messagePtr);
            } else {
                messagePtr->callback = nullptr;
            }
        } else {
            freeMessage(messagePtr);
        }
    }
}
