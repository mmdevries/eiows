#ifndef NODE_UWS_H
#define NODE_UWS_H

#include "Socket.h"
#include <vector>
#include <mutex>

namespace uS {
    class WIN32_EXPORT Node {
        protected:
            Loop *loop;
            NodeData *nodeData;
            std::recursive_mutex asyncMutex;

        public:
            Node(int recvLength = 1024, int prePadding = 0, int postPadding = 0);
            ~Node();

            Loop *getLoop() {
                return loop;
            }
    };
}

#endif // NODE_UWS_H
