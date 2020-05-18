#include "Group.h"
#include "Hub.h"

namespace uWS {

    void Group::setUserData(void *user) {
        this->userData = user;
    }

    void *Group::getUserData() {
        return userData;
    }

    void Group::addWebSocket(WebSocket *webSocket) {
        if (webSocketHead) {
            webSocketHead->prev = webSocket;
            webSocket->next = webSocketHead;
        } else {
            webSocket->next = nullptr;
        }
        webSocketHead = webSocket;
        webSocket->prev = nullptr;
    }

    void Group::removeWebSocket(WebSocket *webSocket) {
        if (iterators.size()) {
            iterators.top() = webSocket->next;
        }
        if (webSocket->prev == webSocket->next) {
            webSocketHead = nullptr;
        } else {
            if (webSocket->prev) {
                ((WebSocket *) webSocket->prev)->next = webSocket->next;
            } else {
                webSocketHead = (WebSocket *) webSocket->next;
            }
            if (webSocket->next) {
                ((WebSocket *) webSocket->next)->prev = webSocket->prev;
            }
        }
    }

    Group::Group(int extensionOptions, unsigned int maxPayload, Hub *hub, uS::NodeData *nodeData) :
        uS::NodeData(*nodeData),
        maxPayload(maxPayload),
        hub(hub),
        extensionOptions(extensionOptions) {
            connectionHandler = [](WebSocket *) {};
            transferHandler = [](WebSocket *) {};
            messageHandler = [](WebSocket *, char *, size_t, OpCode) {};
            disconnectionHandler = [](WebSocket *, int, char *, size_t) {};
            pingHandler = pongHandler = [](WebSocket *, char *, size_t) {};
            errorHandler = [](int, void *) {};

            this->extensionOptions |= CLIENT_NO_CONTEXT_TAKEOVER | SERVER_NO_CONTEXT_TAKEOVER;
        }

    void Group::onConnection(std::function<void (WebSocket *)> handler) {
        connectionHandler = handler;
    }

    void Group::onTransfer(std::function<void (WebSocket *)> handler) {
        transferHandler = handler;
    }

    void Group::onMessage(std::function<void (WebSocket *, char *, size_t, OpCode)> handler) {
        messageHandler = handler;
    }

    void Group::onDisconnection(std::function<void (WebSocket *, int, char *, size_t)> handler) {
        disconnectionHandler = handler;
    }

    void Group::onPing(std::function<void (WebSocket *, char *, size_t)> handler) {
        pingHandler = handler;
    }

    void Group::onPong(std::function<void (WebSocket *, char *, size_t)> handler) {
        pongHandler = handler;
    }

    void Group::onError(std::function<void (int, void *)> handler) {
        errorHandler = handler;
    }

    void Group::close(int code, char *message, size_t length) {
        forEach([code, message, length](uWS::WebSocket *ws) {
                ws->close(code, message, length);
                });
        if (timer) {
            timer->stop();
            timer->close();
        }
    }

}
