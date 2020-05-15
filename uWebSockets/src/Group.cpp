#include "Group.h"
#include "Hub.h"

namespace uWS {

template <bool isServer>
void Group<isServer>::setUserData(void *user) {
    this->userData = user;
}

template <bool isServer>
void *Group<isServer>::getUserData() {
    return userData;
}

template <bool isServer>
void Group<isServer>::addWebSocket(WebSocket<isServer> *webSocket) {
    if (webSocketHead) {
        webSocketHead->prev = webSocket;
        webSocket->next = webSocketHead;
    } else {
        webSocket->next = nullptr;
    }
    webSocketHead = webSocket;
    webSocket->prev = nullptr;
}

template <bool isServer>
void Group<isServer>::removeWebSocket(WebSocket<isServer> *webSocket) {
    if (iterators.size()) {
        iterators.top() = webSocket->next;
    }
    if (webSocket->prev == webSocket->next) {
        webSocketHead = nullptr;
    } else {
        if (webSocket->prev) {
            ((WebSocket<isServer> *) webSocket->prev)->next = webSocket->next;
        } else {
            webSocketHead = (WebSocket<isServer> *) webSocket->next;
        }
        if (webSocket->next) {
            ((WebSocket<isServer> *) webSocket->next)->prev = webSocket->prev;
        }
    }
}

template <bool isServer>
Group<isServer>::Group(int extensionOptions, unsigned int maxPayload, Hub *hub, uS::NodeData *nodeData) : uS::NodeData(*nodeData), maxPayload(maxPayload), hub(hub), extensionOptions(extensionOptions) {
    connectionHandler = [](WebSocket<isServer> *) {};
    transferHandler = [](WebSocket<isServer> *) {};
    messageHandler = [](WebSocket<isServer> *, char *, size_t, OpCode) {};
    disconnectionHandler = [](WebSocket<isServer> *, int, char *, size_t) {};
    pingHandler = pongHandler = [](WebSocket<isServer> *, char *, size_t) {};
    errorHandler = [](errorType) {};

    this->extensionOptions |= CLIENT_NO_CONTEXT_TAKEOVER | SERVER_NO_CONTEXT_TAKEOVER;
}

template <bool isServer>
void Group<isServer>::onConnection(std::function<void (WebSocket<isServer> *)> handler) {
    connectionHandler = handler;
}

template <bool isServer>
void Group<isServer>::onTransfer(std::function<void (WebSocket<isServer> *)> handler) {
    transferHandler = handler;
}

template <bool isServer>
void Group<isServer>::onMessage(std::function<void (WebSocket<isServer> *, char *, size_t, OpCode)> handler) {
    messageHandler = handler;
}

template <bool isServer>
void Group<isServer>::onDisconnection(std::function<void (WebSocket<isServer> *, int, char *, size_t)> handler) {
    disconnectionHandler = handler;
}

template <bool isServer>
void Group<isServer>::onPing(std::function<void (WebSocket<isServer> *, char *, size_t)> handler) {
    pingHandler = handler;
}

template <bool isServer>
void Group<isServer>::onPong(std::function<void (WebSocket<isServer> *, char *, size_t)> handler) {
    pongHandler = handler;
}

template <bool isServer>
void Group<isServer>::onError(std::function<void (typename Group::errorType)> handler) {
    errorHandler = handler;
}

template <bool isServer>
void Group<isServer>::close(int code, char *message, size_t length) {
    forEach([code, message, length](uWS::WebSocket<isServer> *ws) {
        ws->close(code, message, length);
    });
    if (timer) {
        timer->stop();
        timer->close();
    }
}

template struct Group<true>;
template struct Group<false>;

}
