#ifndef NETWORKING_EIOWS_H
#define NETWORKING_EIOWS_H

#define SOCKET_ERROR -1
#define INVALID_SOCKET -1

#include <cerrno>
#include <unistd.h>
#include "Libuv.h"

namespace uS {
    struct Context {
        static void closeSocket(uv_os_sock_t fd) {
            close(fd);
        }

        static bool wouldBlock() {
            return errno == EWOULDBLOCK || errno == EAGAIN;
        }
    };

    struct Socket;

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
