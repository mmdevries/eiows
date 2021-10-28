// the purpose of this header should be to provide SSL and networking wrapped in a common interface
// it should allow cross-platform networking and SSL and also easy usage of mTCP and similar tech
#include <node.h>
#ifndef NETWORKING_UWS_H
#define NETWORKING_UWS_H

#if NODE_MAJOR_VERSION>=17
#define OPENSSL_CONFIGURED_API 0x10100000L
#define OPENSSL_API_COMPAT 0x10100000L
#endif

#ifndef __linux
#define MSG_NOSIGNAL 0
#else
#include <endian.h>
#endif

#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htobe64(x) OSSwapHostToBigInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#endif

#ifdef _WIN32
#define NOMINMAX
#include <WinSock2.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define SHUT_WR SD_SEND
#ifdef __MINGW32__
// Windows has always been tied to LE
#define htobe64(x) __builtin_bswap64(x)
#define be64toh(x) __builtin_bswap64(x)
#else
#define __thread __declspec(thread)
#define htobe64(x) htonll(x)
#define be64toh(x) ntohll(x)
#define pthread_t DWORD
#define pthread_self GetCurrentThreadId
#endif
#define WIN32_EXPORT __declspec(dllexport)

inline void close(SOCKET fd) {closesocket(fd);}
inline int setsockopt(SOCKET fd, int level, int optname, const void *optval, socklen_t optlen) {
    return setsockopt(fd, level, optname, (const char *) optval, optlen);
}
inline SOCKET dup(SOCKET socket) {
    WSAPROTOCOL_INFOW pi;
    if (WSADuplicateSocketW(socket, GetCurrentProcessId(), &pi) == SOCKET_ERROR) {
        return INVALID_SOCKET;
    }
    return WSASocketW(pi.iAddressFamily, pi.iSocketType, pi.iProtocol, &pi, 0, WSA_FLAG_OVERLAPPED);
}
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
#define WIN32_EXPORT
#endif

#include "Libuv.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <csignal>
#include <vector>
#include <string>
#include <mutex>
#include <algorithm>
#include <memory>

namespace uS {
    // todo: mark sockets nonblocking in these functions
    // todo: probably merge this Context with the TLS::Context for same interface for SSL and non-SSL!
    struct Context {
        Context() {}
        ~Context() {}

        static void closeSocket(uv_os_sock_t fd) {
#ifdef _WIN32
            closesocket(fd);
#else
            close(fd);
#endif
        }

        static bool wouldBlock() {
#ifdef _WIN32
            return WSAGetLastError() == WSAEWOULDBLOCK;
#else
            return errno == EWOULDBLOCK;// || errno == EAGAIN;
#endif
        }
    };

    struct Socket;

    // NodeData is like a Context, maybe merge them?
    struct WIN32_EXPORT NodeData {
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
            return (int) ((length >> 4) + bool(length & 15));
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
