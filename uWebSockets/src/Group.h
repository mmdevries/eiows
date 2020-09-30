#ifndef GROUP_UWS_H
#define GROUP_UWS_H

#include "WebSocket.h"
#include "Extensions.h"
#include <functional>
#include <stack>

namespace eioWS {
    enum ListenOptions {
        TRANSFERS
    };

    struct Hub;

    struct WIN32_EXPORT Group : protected uS::NodeData {
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

            // todo: cannot be named user, collides with parent!
            void *userData = nullptr;

            WebSocket *webSocketHead = nullptr;

            void addWebSocket(WebSocket *webSocket);
            void removeWebSocket(WebSocket *webSocket);

            Group(int extensionOptions, unsigned int maxPayload, Hub *hub, uS::NodeData *nodeData);

        public:
            void onConnection(const std::function<void(WebSocket *)> &handler);
            void onMessage(const std::function<void(WebSocket *, char *, size_t, OpCode)> &handler);
            void onDisconnection(const std::function<void(WebSocket *, int code, char *message, size_t length)> &handler);

            void setUserData(void *user);
            void *getUserData();

            void close(int code = 1000, char *message = nullptr, size_t length = 0);

            template <class F>
                void forEach(const F &cb) {
                    uS::Poll *iterator = webSocketHead;
                    iterators.push(iterator);
                    while (iterator) {
                        uS::Poll *lastIterator = iterator;
                        cb(static_cast<WebSocket *>(iterator));
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
