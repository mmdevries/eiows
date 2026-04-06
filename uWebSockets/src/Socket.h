#ifndef SOCKET_EIOWS_H
#define SOCKET_EIOWS_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <algorithm>
#include <cstring>
#include "Networking.h"

namespace uS {
    // perfectly 64 bytes (4 + 60)
    struct Socket : Poll {
        protected:
            struct {
                int poll : 4;
                int shuttingDown : 4;
            } state = {0, false};

            SSL *ssl;
            void *userData = nullptr;
            Timer *timeout = nullptr;
            NodeData *nodeData;
            const int HEADER_LENGTH = 10;
            Socket *(*dataHandler)(Socket *, char *, size_t) = nullptr;
            void (*endHandler)(Socket *) = nullptr;

            struct TimeoutData {
                Socket *socket;
                void (*onTimeout)(Socket *);
            };

            struct Queue {
                struct Message {
                    const char *data;
                    size_t length;
                    int memoryIndex = -1;
                    Message *nextMessage = nullptr;
                    void (*callback)(void *socket, void *data, bool cancelled, void *reserved) = nullptr;
                    void *callbackData = nullptr, *reserved = nullptr;
                };

                Message *head = nullptr, *tail = nullptr;
                Message *pop()
                {
                    Message *message = head;
                    if (!message) {
                        return nullptr;
                    }

                    if ((head = message->nextMessage)) {
                        return message;
                    } else {
                        head = tail = nullptr;
                        return message;
                    }
                }

                bool empty() const {return head == nullptr;}
                Message *front() {return head;}

                void push(Message *message)
                {
                    message->nextMessage = nullptr;
                    if (tail) {
                        tail->nextMessage = message;
                        tail = message;
                    } else {
                        head = message;
                        tail = message;
                    }
                }
            } messageQueue;

            int getPoll() {
                return state.poll;
            }

            int setPoll(int poll) {
                state.poll = poll;
                return poll;
            }

            void setShuttingDown(bool shuttingDown) {
                state.shuttingDown = shuttingDown;
            }

            void changePoll(Socket *socket) {
                change(socket, socket->getPoll());
            }

            static void timeoutHandler(Timer *timer) {
                TimeoutData *timeoutData = static_cast<TimeoutData *>(timer->getData());
                Socket *s = timeoutData->socket;
                void (*onTimeout)(Socket *) = timeoutData->onTimeout;
                timer->setData(nullptr);
                delete timeoutData;
                s->cancelTimeout();
                onTimeout(s);
            }

            void startTimeout(void (*onTimeout)(Socket *), int timeoutMs = 15000) {
                if (timeout) {
                    cancelTimeout();
                }
                Timer *timer = new Timer(nodeData->loop);
                timer->setData(new TimeoutData{this, onTimeout});
                timer->start(timeoutHandler, timeoutMs, 0);
                timeout = timer;
            }

            void cancelTimeout() {
                Timer *timer = timeout;
                if (timer) {
                    if (timer->getData()) {
                        delete static_cast<TimeoutData *>(timer->getData());
                        timer->setData(nullptr);
                    }
                    timer->stop();
                    timer->close();
                    timeout = nullptr;
                }
            }

            static void sslIoHandler(Poll *p, int status, int events) {
                Socket *socket = static_cast<Socket *>(p);

                if (status < 0) {
                    socket->endHandler(static_cast<Socket *>(p));
                    return;
                }

                if (!socket->messageQueue.empty() && ((events & UV_WRITABLE) || SSL_want(socket->ssl) == SSL_READING)) {
                    while (true) {
                        Queue::Message *messagePtr = socket->messageQueue.front();
                        ssize_t sent = SSL_write(socket->ssl, messagePtr->data, static_cast<int>(messagePtr->length));
                        if (sent == (ssize_t) messagePtr->length) {
                            if (messagePtr->callback) {
                                messagePtr->callback(p, messagePtr->callbackData, false, messagePtr->reserved);
                            }
                            socket->freeMessage(socket->messageQueue.pop());
                            if (socket->messageQueue.empty()) {
                                if ((socket->state.poll & UV_WRITABLE) && SSL_want(socket->ssl) != SSL_WRITING) {
                                    socket->change(socket, socket->setPoll(UV_READABLE));
                                }
                                break;
                            }
                        } else if (sent <= 0) {
                            switch (SSL_get_error(socket->ssl, sent)) {
                                case SSL_ERROR_WANT_READ:
                                    break;
                                case SSL_ERROR_WANT_WRITE:
                                    if ((socket->getPoll() & UV_WRITABLE) == 0) {
                                        socket->change(socket, socket->setPoll(socket->getPoll() | UV_WRITABLE));
                                    }
                                    break;
                                case SSL_ERROR_SSL:
                                case SSL_ERROR_SYSCALL:
                                    ERR_clear_error();
                                    socket->endHandler(static_cast<Socket *>(p));
                                    return;
                                default:
                                    socket->endHandler(static_cast<Socket *>(p));
                                    return;
                            }
                            break;
                        }
                    }
                }

                if (events & UV_READABLE) {
                    do {
                        int length = SSL_read(socket->ssl, socket->nodeData->recvBuffer, socket->nodeData->recvLength);
                        if (length <= 0) {
                            switch (SSL_get_error(socket->ssl, length)) {
                                case SSL_ERROR_WANT_READ:
                                    break;
                                case SSL_ERROR_WANT_WRITE:
                                    if ((socket->getPoll() & UV_WRITABLE) == 0) {
                                        socket->change(socket, socket->setPoll(socket->getPoll() | UV_WRITABLE));
                                    }
                                    break;
                                case SSL_ERROR_SSL:
                                case SSL_ERROR_SYSCALL:
                                    ERR_clear_error();
                                    socket->endHandler(static_cast<Socket *>(p));
                                    return;
                                default:
                                    socket->endHandler(static_cast<Socket *>(p));
                                    return;
                            }
                            break;
                        } else {
                            socket = socket->dataHandler(static_cast<Socket *>(p), socket->nodeData->recvBuffer, length);
                            if (socket->isClosed() || socket->isShuttingDown()) {
                                return;
                            }
                        }
                    } while (SSL_pending(socket->ssl));
                }
            }

            static void ioHandler(Poll *p, int status, int events) {
                Socket *socket = static_cast<Socket *>(p);
                NodeData *nodeData = socket->nodeData;
                Context *netContext = nodeData->netContext;

                if (status < 0) {
                    socket->endHandler(static_cast<Socket *>(p));
                    return;
                }

                if (events & UV_WRITABLE) {
                    if (!socket->messageQueue.empty() && (events & UV_WRITABLE)) {
                        while (true) {
                            Queue::Message *messagePtr = socket->messageQueue.front();
                            ssize_t sent = ::send(socket->getFd(), messagePtr->data, messagePtr->length, MSG_NOSIGNAL);
                            if (sent == (ssize_t) messagePtr->length) {
                                if (messagePtr->callback) {
                                    messagePtr->callback(p, messagePtr->callbackData, false, messagePtr->reserved);
                                }
                                socket->freeMessage(socket->messageQueue.pop());
                                if (socket->messageQueue.empty()) {
                                    socket->change(socket, socket->setPoll(UV_READABLE));
                                    break;
                                }
                            } else if (sent == SOCKET_ERROR) {
                                if (!netContext->wouldBlock()) {
                                    socket->endHandler(static_cast<Socket *>(p));
                                    return;
                                }
                                break;
                            } else {
                                messagePtr->length -= sent;
                                messagePtr->data += sent;
                                break;
                            }
                        }
                    }
                }

                if (events & UV_READABLE) {
                    int length = static_cast<int>(recv(socket->getFd(), nodeData->recvBuffer, nodeData->recvLength, 0));
                    if (length > 0) {
                        socket->dataHandler(static_cast<Socket *>(p), nodeData->recvBuffer, length);
                    } else if (length <= 0 || (length == SOCKET_ERROR && !netContext->wouldBlock())) {
                        socket->endHandler(static_cast<Socket *>(p));
                    }
                }
            }

            void setState(Socket *(*onData)(Socket *, char *, size_t), void (*onEnd)(Socket *)) {
                dataHandler = onData;
                endHandler = onEnd;
                if (ssl) {
                    setCb(sslIoHandler);
                } else {
                    setCb(ioHandler);
                }
            }

            bool hasEmptyQueue() const {
                return messageQueue.empty();
            }

            void enqueue(Queue::Message *message) {
                messageQueue.push(message);
            }

            Queue::Message *allocMessage(size_t length, const char *data = 0) {
                Queue::Message *messagePtr = (Queue::Message *) new char[sizeof(Queue::Message) + length];
                messagePtr->length = length;
                messagePtr->memoryIndex = -1;
                messagePtr->data = (reinterpret_cast<char *>(messagePtr)) + sizeof(Queue::Message);
                messagePtr->nextMessage = nullptr;
                messagePtr->callback = nullptr;
                messagePtr->callbackData = nullptr;
                messagePtr->reserved = nullptr;

                if (data) {
                    memcpy(const_cast<char *>(messagePtr->data), data, messagePtr->length);
                }

                return messagePtr;
            }

            Queue::Message *allocSmallMessage(size_t length) {
                int memoryLength = static_cast<int>(sizeof(Queue::Message) + length);
                int memoryIndex = nodeData->getMemoryBlockIndex(memoryLength);
                Queue::Message *messagePtr = reinterpret_cast<Queue::Message *>(nodeData->getSmallMemoryBlock(memoryIndex));
                messagePtr->length = length;
                messagePtr->memoryIndex = memoryIndex;
                messagePtr->data = (reinterpret_cast<char *>(messagePtr)) + sizeof(Queue::Message);
                messagePtr->nextMessage = nullptr;
                messagePtr->callback = nullptr;
                messagePtr->callbackData = nullptr;
                messagePtr->reserved = nullptr;
                return messagePtr;
            }

            Queue::Message *allocMessageForPayload(size_t length) {
                if (sizeof(Queue::Message) + length <= static_cast<size_t>(uS::NodeData::preAllocMaxSize)) {
                    return allocSmallMessage(length);
                }
                return allocMessage(length);
            }

            void freeMessage(Queue::Message *message) {
                if (message->memoryIndex >= 0) {
                    nodeData->freeSmallMemoryBlock(reinterpret_cast<char *>(message), message->memoryIndex);
                } else {
                    delete [] reinterpret_cast<char *>(message);
                }
            }

            bool write(Queue::Message *message, bool &waiting) {

                if (messageQueue.empty()) {
                    ssize_t sent = 0;
                    if (ssl) {
                        sent = SSL_write(ssl, message->data, static_cast<int>(message->length));
                        if (sent == (ssize_t) message->length) {
                            waiting = false;
                            return true;
                        } else if (sent < 0) {
                            switch (SSL_get_error(ssl, static_cast<int>(sent))) {
                                case SSL_ERROR_WANT_READ:
                                    break;
                                case SSL_ERROR_WANT_WRITE:
                                    if ((getPoll() & UV_WRITABLE) == 0) {
                                        setPoll(getPoll() | UV_WRITABLE);
                                        changePoll(this);
                                    }
                                    break;
                                case SSL_ERROR_SSL:
                                case SSL_ERROR_SYSCALL:
                                    ERR_clear_error();
                                    return false;
                                default:
                                    return false;
                            }
                        }
                    } else {
                        sent = ::send(getFd(), message->data, message->length, MSG_NOSIGNAL);
                        if (sent == (ssize_t) message->length) {
                            waiting = false;
                            return true;
                        } else if (sent == SOCKET_ERROR) {
                            if (!nodeData->netContext->wouldBlock()) {
                                return false;
                            }
                        } else {
                            message->length -= sent;
                            message->data += sent;
                        }

                        if ((getPoll() & UV_WRITABLE) == 0) {
                            setPoll(getPoll() | UV_WRITABLE);
                            changePoll(this);
                        }
                    }
                }
                messageQueue.push(message);
                waiting = true;
                return true;
            }

            typedef size_t (*TransformCallback)(const char *message, char *dst, size_t length, void *transformData);

            void sendTransformed(const char *message, size_t length, TransformCallback transform, void *transformData, void(*callback)(void *socket, void *data, bool cancelled, void *reserved), void *callbackData) {
                size_t estimatedLength = length + HEADER_LENGTH + sizeof(Queue::Message);

                if (hasEmptyQueue()) {
                    Queue::Message *messagePtr = allocMessageForPayload(estimatedLength - sizeof(Queue::Message));
                    messagePtr->length = transform(message, const_cast<char *>(messagePtr->data), length, transformData);

                    bool waiting;
                    if (write(messagePtr, waiting)) {
                        if (!waiting) {
                            freeMessage(messagePtr);
                            if (callback) {
                                callback(this, callbackData, false, nullptr);
                            }
                        } else {
                            messagePtr->callback = callback;
                            messagePtr->callbackData = callbackData;
                        }
                    } else {
                        freeMessage(messagePtr);
                        if (callback) {
                            callback(this, callbackData, true, nullptr);
                        }
                    }
                } else {
                    Queue::Message *messagePtr = allocMessageForPayload(estimatedLength - sizeof(Queue::Message));
                    messagePtr->length = transform(message, const_cast<char *>(messagePtr->data), length, transformData);
                    messagePtr->callback = callback;
                    messagePtr->callbackData = callbackData;
                    enqueue(messagePtr);
                }
            }

        public:
            Socket(NodeData *nodeData, Loop *loop, uv_os_sock_t fd, SSL *ssl) : Poll(loop, fd), ssl(ssl), nodeData(nodeData) {
                if (ssl) {
                    // OpenSSL treats SOCKETs as int
                    SSL_set_fd(ssl, static_cast<int>(fd));
                    SSL_set_mode(ssl, SSL_MODE_RELEASE_BUFFERS);
                }
            }

            NodeData *getNodeData() {
                return nodeData;
            }

            Poll *next = nullptr, *prev = nullptr;

            void *getUserData() {
                return userData;
            }

            void setUserData(void *user) {
                this->userData = user;
            }

            struct Address {
                unsigned int port;
                const char *address;
                const char *family;
            };

            Address getAddress() const;

            void setNoDelay(int enable) const {
                setsockopt(getFd(), IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(int));
            }

            void shutdown() {
                if (ssl) {
                    //todo: poll in/out - have the io_cb recall shutdown if failed
                    SSL_shutdown(ssl);
                } else {
                    ::shutdown(getFd(), SHUT_WR);
                }
            }

            void closeSocket(void (*deleter)(Poll *)) {
                uv_os_sock_t fd = getFd();
                Context *netContext = nodeData->netContext;
                stop();
                netContext->closeSocket(fd);

                if (ssl) {
                    SSL_free(ssl);
                }

                Poll::close(this, deleter);
            }

            bool isShuttingDown() {
                return state.shuttingDown;
            }

            friend class Node;
            friend struct NodeData;
    };
}

#endif // SOCKET_EIOWS_H
