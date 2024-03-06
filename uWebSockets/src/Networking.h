// the purpose of this header should be to provide SSL and networking wrapped in a common interface
// it should allow cross-platform networking and SSL and also easy usage of mTCP and similar tech
#ifndef NETWORKING_UWS_H
#define NETWORKING_UWS_H

#define SOCKET_ERROR -1
#define INVALID_SOCKET -1

#include <unistd.h>
#include <mutex>
#include <vector>
#include <algorithm>
#include "Libuv.h"

namespace uS {
    // todo: mark sockets nonblocking in these functions
    // todo: probably merge this Context with the TLS::Context for same interface for SSL and non-SSL!
    struct Context {
        Context() {}
        ~Context() {}

        static void closeSocket(uv_os_sock_t fd) {
            close(fd);
        }

        static bool wouldBlock() {
            return errno == EWOULDBLOCK;// || errno == EAGAIN;
        }
    };

    struct Socket;

    // NodeData is like a Context, maybe merge them?
    struct NodeData {
        char *recvBufferMemoryBlock;
        char *recvBuffer;
        int recvLength;
        Loop *loop;
        uS::Context *netContext;
        void *user = nullptr;
        static const int preAllocMaxSize = 1024;
        char **preAlloc;

        Async *async = nullptr;
        pthread_t tid;

        std::recursive_mutex *asyncMutex;
        std::vector<Poll *> transferQueue;
        std::vector<Poll *> changePollQueue;
        static void asyncCallback(Async *async);

        static int getMemoryBlockIndex(size_t length) {
            return static_cast<int>((length >> 4) + static_cast<bool>(length & 15));
        }

        char *getSmallMemoryBlock(int index) {
            if (preAlloc[index]) {
                char *memory = preAlloc[index];
                preAlloc[index] = nullptr;
                return memory;
            } else {
                return new char[index << 4];
            }
        }

        void freeSmallMemoryBlock(char *memory, int index) {
            if (!preAlloc[index]) {
                preAlloc[index] = memory;
            } else {
                delete [] memory;
            }
        }

        public:
        void clearPendingPollChanges(Poll *p) {
            asyncMutex->lock();
            changePollQueue.erase(
                std::remove(changePollQueue.begin(), changePollQueue.end(), p),
                changePollQueue.end()
            );
            asyncMutex->unlock();
        }
    };
}

#endif // NETWORKING_UWS_H
