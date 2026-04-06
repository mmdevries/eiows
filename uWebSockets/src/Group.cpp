#include "Group.h"

namespace eioWS {
    void Group::setUserData(void *user, void (*deleter)(void *)) {
        this->userData = user;
        this->userDataDeleter = deleter;
    }

    void *Group::getUserData() {
        return userData;
    }

    void Group::destroy() {
        if (userData && userDataDeleter) {
            userDataDeleter(userData);
            userData = nullptr;
        }
        delete this;
    }

    void Group::addWebSocket(WebSocket *webSocket) {
        liveWebSockets++;
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

    Group::Group(int extensionOptions, unsigned int maxPayload, Hub *hub, uS::NodeData *nodeData, bool owned) :
        uS::NodeData(*nodeData),
        maxPayload(maxPayload),
        hub(hub),
        extensionOptions(extensionOptions),
        owned(owned) {
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

    void Group::markForDeletion() {
        deleteOnDrain = true;
        if (owned && !liveWebSockets) {
            destroy();
        }
    }

    void Group::onSocketClosed() {
        if (liveWebSockets) {
            liveWebSockets--;
        }
        if (owned && deleteOnDrain && !liveWebSockets) {
            destroy();
        }
    }

    void Group::close(int code, char *message, size_t length) {
        uS::Poll *iterator = webSocketHead;
        iterators.push(iterator);
        while (iterator) {
            uS::Poll *lastIterator = iterator;
            eioWS::WebSocket *ws = static_cast<eioWS::WebSocket *>(iterator);
            ws->close(code, message, length);
            iterator = iterators.top();
            if (lastIterator == iterator) {
                iterator = static_cast<uS::Socket *>(iterator)->next;
                iterators.top() = iterator;
            }
        }
        iterators.pop();
    }
}
