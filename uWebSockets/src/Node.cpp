#include "Node.h"

namespace uS {
    Node::Node(int recvLength, int prePadding, int postPadding, bool useDefaultLoop) {
        nodeData = new NodeData;
        nodeData->recvBufferMemoryBlock = new char[recvLength];
        nodeData->recvBuffer = nodeData->recvBufferMemoryBlock + prePadding;
        nodeData->recvLength = recvLength - prePadding - postPadding;

        nodeData->tid = pthread_self();
        loop = Loop::createLoop(useDefaultLoop);

        // each node has a context
        nodeData->netContext = new Context();

        nodeData->loop = loop;
        nodeData->asyncMutex = &asyncMutex;

        int indices = NodeData::getMemoryBlockIndex(NodeData::preAllocMaxSize) + 1;
        nodeData->preAlloc = new char*[indices];
        for (int i = 0; i < indices; i++) {
            nodeData->preAlloc[i] = nullptr;
        }

#if (NODE_MAJOR_VERSION>=10)
        nodeData->clientContext = SSL_CTX_new(TLS_method());
        SSL_CTX_set_min_proto_version(nodeData->clientContext, TLS1_VERSION);
        SSL_CTX_set_max_proto_version(nodeData->clientContext, TLS1_3_VERSION);
#else
        nodeData->clientContext = SSL_CTX_new(SSLv23_client_method());
        SSL_CTX_set_options(nodeData->clientContext, SSL_OP_NO_SSLv3);
#endif
    }

    Node::~Node() {
        delete [] nodeData->recvBufferMemoryBlock;
        SSL_CTX_free(nodeData->clientContext);

        int indices = NodeData::getMemoryBlockIndex(NodeData::preAllocMaxSize) + 1;
        for (int i = 0; i < indices; i++) {
            if (nodeData->preAlloc[i]) {
                delete [] nodeData->preAlloc[i];
            }
        }
        delete [] nodeData->preAlloc;
        delete nodeData->netContext;
        delete nodeData;
        loop->destroy();
    }
}
