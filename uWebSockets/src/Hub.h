#ifndef HUB_UWS_H
#define HUB_UWS_H

#include "Group.h"
#include "Node.h"
#include <string>
#include <zlib.h>
#include <mutex>
#include <map>

namespace uWS {

    struct WIN32_EXPORT Hub : protected uS::Node, public Group<SERVER> {
        protected:
            struct ConnectionData {
                std::string path;
                void *user;
            };

            static z_stream *allocateDefaultCompressor(z_stream *zStream);

            z_stream inflationStream = {}, deflationStream = {};
            char *deflate(char *data, size_t &length, z_stream *slidingDeflateWindow);
            char *inflate(char *data, size_t &length, size_t maxPayload);
            char *zlibBuffer;
            std::string dynamicZlibBuffer;
            static const int LARGE_BUFFER_SIZE = 300 * 1024;


        public:
            template <bool isServer>
                Group<isServer> *createGroup(int extensionOptions = 0, unsigned int maxPayload = 16777216) {
                    return new Group<isServer>(extensionOptions, maxPayload, this, nodeData);
                }

            template <bool isServer>
                Group<isServer> &getDefaultGroup() {
                    return static_cast<Group<isServer> &>(*this);
                }

            void upgrade(uv_os_sock_t fd, const char *secKey, SSL *ssl, const char *extensions, size_t extensionsLength, const char *subprotocol, size_t subprotocolLength, Group<SERVER> *serverGroup = nullptr);

            Hub(int extensionOptions = 0, bool useDefaultLoop = false, unsigned int maxPayload = 16777216) : 
                uS::Node(LARGE_BUFFER_SIZE, WebSocketProtocol<WebSocket<SERVER>>::CONSUME_PRE_PADDING, WebSocketProtocol<WebSocket<SERVER>>::CONSUME_POST_PADDING, useDefaultLoop),
                Group<SERVER>(extensionOptions, maxPayload, this, nodeData) {
                    inflateInit2(&inflationStream, -15);
                    zlibBuffer = new char[LARGE_BUFFER_SIZE];
                    allocateDefaultCompressor(&deflationStream);
                }

            ~Hub() {
                inflateEnd(&inflationStream);
                deflateEnd(&deflationStream);
                delete [] zlibBuffer;
            }

            using uS::Node::getLoop;
            using Group<SERVER>::onConnection;
            using Group<SERVER>::onTransfer;
            using Group<SERVER>::onMessage;
            using Group<SERVER>::onDisconnection;
            using Group<SERVER>::onPing;
            using Group<SERVER>::onPong;
            using Group<SERVER>::onError;

            friend struct WebSocket<SERVER>;
            friend struct WebSocket<CLIENT>;
    };

}

#endif // HUB_UWS_H
