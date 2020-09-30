#ifndef WEBSOCKET_UWS_H
#define WEBSOCKET_UWS_H

#include "WebSocketProtocol.h"
#include "Socket.h"


namespace eioWS {
    struct Group;

    struct WIN32_EXPORT WebSocket : uS::Socket, WebSocketState {
        protected:
            unsigned int maxPayload;
            std::string fragmentBuffer;
            enum CompressionStatus : char {
                DISABLED,
                ENABLED,
                COMPRESSED_FRAME
            } compressionStatus;
            unsigned char controlTipLength = 0, hasOutstandingPong = false;

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

            static bool handleFragment(char *data, size_t length, unsigned int remainingBytes, int opCode, bool fin, WebSocketState *webSocketState);

            void upgrade(const char *secKey, const std::string& extensionsResponse, const char *subprotocol, size_t subprotocolLength);

        public:
            struct PreparedMessage {
                char *buffer;
                size_t length;
                int references;
                void(*callback)(void *webSocket, void *data, bool cancelled, void *reserved);
            };

            void close(int code = 1000, const char *message = nullptr, size_t length = 0);
            void terminate();
            void ping(const char *message) {send(message, OpCode::PING);}
            void send(const char *message, OpCode opCode = OpCode::TEXT) {send(message, strlen(message), opCode);}
            void send(const char *message, size_t length, OpCode opCode, void(*callback)(WebSocket *webSocket, void *data, bool cancelled, void *reserved) = nullptr, void *callbackData = nullptr, bool compress = false);

            friend struct Hub;
            friend struct Group;
            friend struct uS::Socket;
            friend class WebSocketProtocol<WebSocket>;
    };
}

#endif // WEBSOCKET_UWS_H
