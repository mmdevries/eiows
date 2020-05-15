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

template <bool isServer>
struct WIN32_EXPORT Group : protected uS::NodeData {
protected:
    friend struct Hub;
    friend struct WebSocket<isServer>;

    std::function<void(WebSocket<isServer> *)> connectionHandler;
    std::function<void(WebSocket<isServer> *)> transferHandler;
    std::function<void(WebSocket<isServer> *, char *message, size_t length, OpCode opCode)> messageHandler;
    std::function<void(WebSocket<isServer> *, int code, char *message, size_t length)> disconnectionHandler;
    std::function<void(WebSocket<isServer> *, char *, size_t)> pingHandler;

    using errorType = typename std::conditional<isServer, int, void *>::type;
    std::function<void(errorType)> errorHandler;

    unsigned int maxPayload;
    Hub *hub;
    int extensionOptions;
    uS::Timer *timer = nullptr;
    const char *userPingMessage;
    size_t userPingMessageLength;
    OpCode pingMessageType;
    std::stack<uS::Poll *> iterators;

    // todo: cannot be named user, collides with parent!
    void *userData = nullptr;

    WebSocket<isServer> *webSocketHead = nullptr;

    void addWebSocket(WebSocket<isServer> *webSocket);
    void removeWebSocket(WebSocket<isServer> *webSocket);

    Group(int extensionOptions, unsigned int maxPayload, Hub *hub, uS::NodeData *nodeData);

public:
    std::function<void(WebSocket<isServer> *, char *, size_t)> pongHandler;

    void onConnection(std::function<void(WebSocket<isServer> *)> handler);
    void onTransfer(std::function<void(WebSocket<isServer> *)> handler);
    void onMessage(std::function<void(WebSocket<isServer> *, char *, size_t, OpCode)> handler);
    void onDisconnection(std::function<void(WebSocket<isServer> *, int code, char *message, size_t length)> handler);
    void onPing(std::function<void(WebSocket<isServer> *, char *, size_t)> handler);
    void onPong(std::function<void(WebSocket<isServer> *, char *, size_t)> handler);
    void onError(std::function<void(errorType)> handler);

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
            cb((WebSocket<isServer> *) iterator);
            iterator = iterators.top();
            if (lastIterator == iterator) {
                iterator = ((uS::Socket *) iterator)->next;
                iterators.top() = iterator;
            }
        }
        iterators.pop();
    }

    static Group<isServer> *from(uS::Socket *s) {
        return static_cast<Group<isServer> *>(s->getNodeData());
    }
};

}

#endif // GROUP_UWS_H
