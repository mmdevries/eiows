#ifndef WEBSOCKET_EIOWS_H
#define WEBSOCKET_EIOWS_H

#include <string>
#include "WebSocketProtocol.h"
#include "Socket.h"

namespace eioWS {
    struct Group;

    struct WebSocket : uS::Socket, WebSocketState {
        protected:
            struct TransformData {
                OpCode opCode;
                bool compress;
                WebSocket *webSocket;
            };
            struct PreparedTransformData {
                OpCode opCode;
                size_t (*writePayload)(char *dst, size_t length, void *data);
                void *data;
            };

            unsigned int maxPayload;
            std::string fragmentBuffer;
            enum CompressionStatus : char {
                DISABLED,
                ENABLED,
                COMPRESSED_FRAME
            } compressionStatus;
            unsigned char controlTipLength = 0;

            void *slidingDeflateWindow = nullptr;

            WebSocket(unsigned int maxP, bool perMessageDeflate, uS::Socket *socket);

            static uS::Socket *onData(uS::Socket *s, char *data, size_t length);
            static void onEnd(uS::Socket *s);
            using uS::Socket::closeSocket;

            static bool refusePayloadLength(uint64_t length, WebSocketState *webSocketState) {
                WebSocket *webSocket = static_cast<WebSocket *>(webSocketState);
                return length > webSocket->maxPayload;
            }

            static bool setCompressed(WebSocketState *webSocketState) {
                WebSocket *webSocket = static_cast<WebSocket *>(webSocketState);

                if (webSocket->compressionStatus == WebSocket::CompressionStatus::ENABLED) {
                    webSocket->compressionStatus = WebSocket::CompressionStatus::COMPRESSED_FRAME;
                    return true;
                } else {
                    return false;
                }
            }

            static void forceClose(WebSocketState *webSocketState) {
                WebSocket *webSocket = static_cast<WebSocket *>(webSocketState);
                webSocket->terminate();
            }

            static size_t transformMessage(const char *src, char *dst, size_t length, void *transformData);
            static size_t transformPreparedMessage(char *dst, size_t length, void *transformData);
            static void deleteSocket(uS::Poll *p);
            static bool handleFragment(char *data, size_t length, unsigned int remainingBytes, int opCode, bool fin, WebSocketState *webSocketState);
            static const WebSocketProtocolHooks protocolHooks;

            void upgrade(const char *secKey, const std::string& extensionsResponse, const char *subprotocol, size_t subprotocolLength);

        public:
            void close(int code = 1000, const char *message = nullptr, size_t length = 0);
            void terminate();
            void setState() { uS::Socket::setState(onData, onEnd); }
            void send(const char *message, OpCode opCode = OpCode::TEXT) {send(message, strlen(message), opCode);}
            void send(const char *message, size_t length, OpCode opCode, void(*callback)(WebSocket *webSocket, void *data, bool cancelled, void *reserved) = nullptr, void *callbackData = nullptr, bool compress = false);
            void sendPrepared(size_t length, OpCode opCode, size_t (*writePayload)(char *dst, size_t length, void *data), void *data, void(*callback)(WebSocket *webSocket, void *data, bool cancelled, void *reserved) = nullptr, void *callbackData = nullptr);

            friend struct Hub;
            friend struct Group;
            friend struct uS::Socket;
    };
}

#endif // WEBSOCKET_EIOWS_H
