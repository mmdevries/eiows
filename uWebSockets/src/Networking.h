// the purpose of this header should be to provide SSL and networking wrapped in a common interface
// it should allow cross-platform networking and SSL and also easy usage of mTCP and similar tech
#ifndef NETWORKING_EIOWS_H
#define NETWORKING_EIOWS_H

#define SOCKET_ERROR -1
#define INVALID_SOCKET -1

#include <cerrno>
#include <unistd.h>
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
        static const int preAllocMaxSize = 8192;
        char **preAlloc;

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
    };
}

#endif // NETWORKING_EIOWS_H
