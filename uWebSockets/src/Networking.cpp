#include "Networking.h"

namespace uS {
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
