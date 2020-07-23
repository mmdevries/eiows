#include "Node.h"

namespace uS {
    Node::Node(int recvLength, int prePadding, int postPadding) {
        nodeData = new NodeData;
        nodeData->recvBufferMemoryBlock = new char[recvLength];
        nodeData->recvBuffer = nodeData->recvBufferMemoryBlock + prePadding;
        nodeData->recvLength = recvLength - prePadding - postPadding;

        nodeData->tid = pthread_self();
        loop = Loop::createLoop();

        // each node has a context
        nodeData->netContext = new Context();
        nodeData->loop = loop;

        nodeData->asyncMutex = &asyncMutex;

        int indices = NodeData::getMemoryBlockIndex(NodeData::preAllocMaxSize) + 1;
        nodeData->preAlloc = new char*[indices];
        for (int i = 0; i < indices; i++) {
            nodeData->preAlloc[i] = nullptr;
        }
    }

    Node::~Node() {
        delete [] nodeData->recvBufferMemoryBlock;

        int indices = NodeData::getMemoryBlockIndex(NodeData::preAllocMaxSize) + 1;
        for (int i = 0; i < indices; i++) {
            if (nodeData->preAlloc[i]) {
                delete [] nodeData->preAlloc[i];
            }
        }
        delete [] nodeData->preAlloc;
        delete nodeData->netContext;
        delete nodeData;
    }
}
