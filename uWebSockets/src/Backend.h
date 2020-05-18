#ifndef BACKEND_H
#define BACKEND_H

// Default to Libuv if nothing specified and not on Linux
#if !defined(__linux__) || defined(USE_LIBUV)
#include "Libuv.h"
#endif

#endif // BACKEND_H
