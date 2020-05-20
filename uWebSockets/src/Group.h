#ifndef GROUP_UWS_H
#define GROUP_UWS_H

#include "WebSocket.h"
#include "Extensions.h"
#include <functional>
#include <stack>

namespace uWS {

    enum ListenOptions {
        TRANSFERS
    };

    struct Hub;

    struct WIN32_EXPORT Group : protected uS::NodeData {
        protected:
            friend struct Hub;
            friend struct WebSocket;

            std::function<void(WebSocket *)> connectionHandler;
            std::function<void(WebSocket *)> transferHandler;
            std::function<void(WebSocket *, char *message, size_t length, OpCode opCode)> messageHandler;
            std::function<void(WebSocket *, int code, char *message, size_t length)> disconnectionHandler;
            std::function<void(WebSocket *, char *, size_t)> pingHandler;
            std::function<void(int, void *)> errorHandler;

            unsigned int maxPayload;
            Hub *hub;
            int extensionOptions;
            const char *userPingMessage;
            size_t userPingMessageLength;
            OpCode pingMessageType;
            std::stack<uS::Poll *> iterators;

            // todo: cannot be named user, collides with parent!
            void *userData = nullptr;

            WebSocket *webSocketHead = nullptr;

            void addWebSocket(WebSocket *webSocket);
            void removeWebSocket(WebSocket *webSocket);

            Group(int extensionOptions, unsigned int maxPayload, Hub *hub, uS::NodeData *nodeData);

        public:
            std::function<void(WebSocket *, char *, size_t)> pongHandler;

            void onConnection(const std::function<void(WebSocket *)> &handler);
            void onMessage(const std::function<void(WebSocket *, char *, size_t, OpCode)> &handler);
            void onDisconnection(const std::function<void(WebSocket *, int code, char *message, size_t length)> &handler);

            // Thread safe
            void setUserData(void *user);
            void *getUserData();

            // Not thread safe
            void close(int code = 1000, char *message = nullptr, size_t length = 0);

            template <class F>
                void forEach(const F &cb) {
                    uS::Poll *iterator = webSocketHead;
                    iterators.push(iterator);
                    while (iterator) {
                        uS::Poll *lastIterator = iterator;
                        cb((WebSocket *) iterator);
                        iterator = iterators.top();
                        if (lastIterator == iterator) {
                            iterator = ((uS::Socket *) iterator)->next;
                            iterators.top() = iterator;
                        }
                    }
                    iterators.pop();
                }

            static Group *from(uS::Socket *s) {
                return static_cast<Group *>(s->getNodeData());
            }
    };

}

#endif // GROUP_UWS_H
