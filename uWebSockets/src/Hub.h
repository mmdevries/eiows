#ifndef HUB_UWS_H
#define HUB_UWS_H

#include "Group.h"
#include "Node.h"
#include <string>
#include <zlib.h>
#include <mutex>
#include <map>

namespace eioWS {
    struct WIN32_EXPORT Hub : protected uS::Node, public Group {
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
            Group *createGroup(int extensionOptions = 0, unsigned int maxPayload = 16777216) {
                return new Group(extensionOptions, maxPayload, this, nodeData);
            }

            Group &getDefaultGroup() {
                return static_cast<Group &>(*this);
            }

            void upgrade(uv_os_sock_t fd, const char *secKey, SSL *ssl, const char *extensions, size_t extensionsLength, const char *subprotocol, size_t subprotocolLength, Group *serverGroup = nullptr);

            Hub(int extensionOptions = 0, unsigned int maxPayload = 16777216) :
                uS::Node(LARGE_BUFFER_SIZE, WebSocketProtocol<WebSocket>::CONSUME_PRE_PADDING, WebSocketProtocol<WebSocket>::CONSUME_POST_PADDING),
                Group(extensionOptions, maxPayload, this, nodeData) {
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
            using Group::onConnection;
            using Group::onMessage;
            using Group::onDisconnection;

            friend struct WebSocket;
    };
}

#endif // HUB_UWS_H
