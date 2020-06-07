#include "Group.h"
#include "Hub.h"

namespace eioWS {
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
                (static_cast<WebSocket *>(webSocket->prev))->next = webSocket->next;
            } else {
                webSocketHead = static_cast<WebSocket *>(webSocket->next);
            }
            if (webSocket->next) {
                (static_cast<WebSocket *>(webSocket->next))->prev = webSocket->prev;
            }
        }
    }

    Group::Group(int extensionOptions, unsigned int maxPayload, Hub *hub, uS::NodeData *nodeData) :
        uS::NodeData(*nodeData),
        maxPayload(maxPayload),
        hub(hub),
        extensionOptions(extensionOptions) {
            this->extensionOptions |= CLIENT_NO_CONTEXT_TAKEOVER | SERVER_NO_CONTEXT_TAKEOVER;
        }

    void Group::onConnection(const std::function<void (WebSocket *)> &handler) {
        connectionHandler = handler;
    }

    void Group::onMessage(const std::function<void (WebSocket *, char *, size_t, OpCode)> &handler) {
        messageHandler = handler;
    }

    void Group::onDisconnection(const std::function<void (WebSocket *, int, char *, size_t)> &handler) {
        disconnectionHandler = handler;
    }

    void Group::close(int code, char *message, size_t length) {
        forEach([code, message, length](eioWS::WebSocket *ws) {
            ws->close(code, message, length);
        });
    }
}
