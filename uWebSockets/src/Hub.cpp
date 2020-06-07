#include "Hub.h"
#include <openssl/sha.h>
#include <string>

namespace eioWS {
    z_stream *Hub::allocateDefaultCompressor(z_stream *zStream) {
        deflateInit2(zStream, 1, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        return zStream;
    }

    char *Hub::deflate(char *data, size_t &length, z_stream *slidingDeflateWindow) {
        dynamicZlibBuffer.clear();

        z_stream *compressor = slidingDeflateWindow ? slidingDeflateWindow : &deflationStream;

        compressor->next_in = (Bytef *) data;
        compressor->avail_in = (unsigned int) length;

        // note: zlib requires more than 6 bytes with Z_SYNC_FLUSH
        const int DEFLATE_OUTPUT_CHUNK = LARGE_BUFFER_SIZE;

        int err;
        do {
            compressor->next_out = (Bytef *) zlibBuffer;
            compressor->avail_out = DEFLATE_OUTPUT_CHUNK;

            err = ::deflate(compressor, Z_SYNC_FLUSH);
            if (Z_OK == err && compressor->avail_out == 0) {
                dynamicZlibBuffer.append(zlibBuffer, DEFLATE_OUTPUT_CHUNK - compressor->avail_out);
                continue;
            } else {
                break;
            }
        } while (true);

        // note: should not change avail_out
        if (!slidingDeflateWindow) {
            deflateReset(compressor);
        }

        if (dynamicZlibBuffer.length()) {
            dynamicZlibBuffer.append(zlibBuffer, DEFLATE_OUTPUT_CHUNK - compressor->avail_out);

            length = dynamicZlibBuffer.length() - 4;
            return (char *) dynamicZlibBuffer.data();
        }

        length = DEFLATE_OUTPUT_CHUNK - compressor->avail_out - 4;
        return zlibBuffer;
    }

    // todo: let's go through this code once more some time!
    char *Hub::inflate(char *data, size_t &length, size_t maxPayload) {
        dynamicZlibBuffer.clear();

        inflationStream.next_in = (Bytef *) data;
        inflationStream.avail_in = (unsigned int) length;

        int err;
        do {
            inflationStream.next_out = (Bytef *) zlibBuffer;
            inflationStream.avail_out = LARGE_BUFFER_SIZE;
            err = ::inflate(&inflationStream, Z_FINISH);
            if (!inflationStream.avail_in) {
                break;
            }

            dynamicZlibBuffer.append(zlibBuffer, LARGE_BUFFER_SIZE - inflationStream.avail_out);
        } while (err == Z_BUF_ERROR && dynamicZlibBuffer.length() <= maxPayload);

        inflateReset(&inflationStream);

        if ((err != Z_BUF_ERROR && err != Z_OK) || dynamicZlibBuffer.length() > maxPayload) {
            length = 0;
            return nullptr;
        }

        if (dynamicZlibBuffer.length()) {
            dynamicZlibBuffer.append(zlibBuffer, LARGE_BUFFER_SIZE - inflationStream.avail_out);

            length = dynamicZlibBuffer.length();
            return (char *) dynamicZlibBuffer.data();
        }

        length = LARGE_BUFFER_SIZE - inflationStream.avail_out;
        return zlibBuffer;
    }

    void Hub::upgrade(uv_os_sock_t fd, const char *secKey, SSL *ssl, const char *extensions, size_t extensionsLength, const char *subprotocol, size_t subprotocolLength, Group *serverGroup) {
        if (!serverGroup) {
            serverGroup = &getDefaultGroup();
        }

        uS::Socket s((uS::NodeData *) serverGroup, this->getLoop(), fd, ssl);
        s.setNoDelay(true);

        bool perMessageDeflate = false;
        ExtensionsNegotiator extensionsNegotiator(serverGroup->extensionOptions);
        extensionsNegotiator.readOffer(std::string(extensions, extensionsLength));
        std::string extensionsResponse = extensionsNegotiator.generateOffer();
        if (extensionsNegotiator.getNegotiatedOptions() & PERMESSAGE_DEFLATE) {
            perMessageDeflate = true;
        }

        WebSocket *webSocket = new WebSocket(serverGroup->maxPayload, perMessageDeflate, &s);
        webSocket->upgrade(secKey, extensionsResponse, subprotocol, subprotocolLength);

        webSocket->setState<WebSocket>();
        webSocket->change(webSocket, webSocket->setPoll(UV_READABLE));
        serverGroup->addWebSocket(webSocket);
        serverGroup->connectionHandler(webSocket);
    }
}
