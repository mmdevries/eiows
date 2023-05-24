#include "Networking.h"

namespace uS {
    struct Init {
        Init() {signal(SIGPIPE, SIG_IGN);}
    } init;
}
