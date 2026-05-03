#include "Networking.h"
#include <csignal>

namespace {
    struct Init {
        Init() {std::signal(SIGPIPE, SIG_IGN);}
    } init;
}
