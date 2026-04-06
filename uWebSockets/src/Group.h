#ifndef GROUP_EIOWS_H
#define GROUP_EIOWS_H

#include "WebSocket.h"
#include "Extensions.h"
#include <stack>
#include <functional>

namespace eioWS {
    struct Hub;

    struct Group : protected uS::NodeData {
        protected:
            friend struct Hub;
            friend struct WebSocket;

            std::function<void(WebSocket *)> connectionHandler = [](WebSocket *) {};
            std::function<void(WebSocket *, char *message, size_t length, OpCode opCode)> messageHandler = [](WebSocket *, char *, size_t, OpCode) {};
            std::function<void(WebSocket *, int code, char *message, size_t length)> disconnectionHandler = [](WebSocket *, int, char *, size_t) {};

            unsigned int maxPayload;
            Hub *hub;
            int extensionOptions;
            std::stack<uS::Poll *> iterators;
            void *userData = nullptr;
            void (*userDataDeleter)(void *) = nullptr;
            unsigned int liveWebSockets = 0;
            bool owned = false;
            bool deleteOnDrain = false;

            WebSocket *webSocketHead = nullptr;

            void addWebSocket(WebSocket *webSocket);
            void removeWebSocket(WebSocket *webSocket);
            void destroy();

            Group(int extensionOptions, unsigned int maxPayload, Hub *hub, uS::NodeData *nodeData, bool owned = false);

        public:
            void onConnection(const std::function<void(WebSocket *)> &handler);
            void onMessage(const std::function<void(WebSocket *, char *, size_t, OpCode)> &handler);
            void onDisconnection(const std::function<void(WebSocket *, int code, char *message, size_t length)> &handler);

            void setUserData(void *user, void (*deleter)(void *) = nullptr);
            void *getUserData();
            void markForDeletion();
            void onSocketClosed();

            void close(int code = 1000, char *message = nullptr, size_t length = 0);

            static Group *from(uS::Socket *s) {
                return static_cast<Group *>(s->getNodeData());
            }
    };
}

#endif // GROUP_EIOWS_H
