#ifndef NODE_EIOWS_H
#define NODE_EIOWS_H

#include "Networking.h"

namespace uS {
    class Node {
        protected:
            Loop *loop;
            NodeData *nodeData;

        public:
            Node(int recvLength = 1024, int prePadding = 0, int postPadding = 0);
            ~Node();

            Loop *getLoop() {
                return loop;
            }
    };
}

#endif // NODE_EIOWS_H
