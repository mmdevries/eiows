#include "Networking.h"

namespace uS {

namespace TLS {

Context::Context(const Context &other)
{
    if (other.context) {
        context = other.context;
        SSL_CTX_up_ref(context);
    }
}

Context &Context::operator=(const Context &other) {
    if (other.context) {
        context = other.context;
        SSL_CTX_up_ref(context);
    }
    return *this;
}

Context::~Context()
{
    if (context) {
        SSL_CTX_free(context);
    }
}

struct Init {
    Init() {SSL_library_init();}
    ~Init() {/*EVP_cleanup();*/}
} init;

}

#ifndef _WIN32
struct Init {
    Init() {signal(SIGPIPE, SIG_IGN);}
} init;
#endif

#ifdef _WIN32
#pragma comment(lib, "Ws2_32.lib")

struct WindowsInit {
    WSADATA wsaData;
    WindowsInit() {WSAStartup(MAKEWORD(2, 2), &wsaData);}
    ~WindowsInit() {WSACleanup();}
} windowsInit;

#endif

}
