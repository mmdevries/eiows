#include "StreamWebSocket.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace eioWS {

namespace {
constexpr size_t FRAGMENT_BUFFER_RETAIN_LIMIT = 1024 * 1024;
constexpr size_t ZLIB_CHUNK_SIZE = 32 * 1024;
constexpr unsigned char DEFLATE_TRAILER[] = {0x00, 0x00, 0xff, 0xff};

void trimBuffer(std::string &buffer) {
    if (buffer.empty() && buffer.capacity() > FRAGMENT_BUFFER_RETAIN_LIMIT) {
        std::string().swap(buffer);
    }
}
} // namespace

const WebSocketProtocolHooks StreamWebSocket::protocolHooks = {
    StreamWebSocket::refusePayloadLength,
    StreamWebSocket::setCompressed,
    StreamWebSocket::forceClose,
    StreamWebSocket::handleFragment
};

StreamWebSocket::StreamWebSocket(int negotiatedOptions, uint32_t maxPayload) :
    maxPayload(maxPayload),
    extensionOptions(negotiatedOptions),
    compressionStatus((negotiatedOptions & PERMESSAGE_DEFLATE)
        ? CompressionStatus::ENABLED
        : CompressionStatus::DISABLED) {
    if (compressionStatus != CompressionStatus::DISABLED) {
        if (inflateInit2(&inflationStream, -15) != Z_OK) {
            throw std::runtime_error("failed to initialize WebSocket inflater");
        }
        inflaterInitialized = true;

        if (deflateInit2(&deflationStream,
                         1,
                         Z_DEFLATED,
                         -15,
                         8,
                         Z_DEFAULT_STRATEGY) != Z_OK) {
            inflateEnd(&inflationStream);
            inflaterInitialized = false;
            throw std::runtime_error("failed to initialize WebSocket deflater");
        }
        deflaterInitialized = true;
    }
}

StreamWebSocket::~StreamWebSocket() {
    if (inflaterInitialized) {
        inflateEnd(&inflationStream);
    }
    if (deflaterInitialized) {
        deflateEnd(&deflationStream);
    }
}

bool StreamWebSocket::refusePayloadLength(uint64_t length, WebSocketState *state) {
    StreamWebSocket *webSocket = static_cast<StreamWebSocket *>(state);
    if ((webSocket->maxPayload && length > webSocket->maxPayload) ||
        length > std::numeric_limits<unsigned int>::max()) {
        webSocket->failureCode = 1009;
        return true;
    }
    return false;
}

bool StreamWebSocket::setCompressed(WebSocketState *state) {
    StreamWebSocket *webSocket = static_cast<StreamWebSocket *>(state);
    if (webSocket->compressionStatus == CompressionStatus::ENABLED) {
        webSocket->compressionStatus = CompressionStatus::COMPRESSED_FRAME;
        return true;
    }
    return false;
}

void StreamWebSocket::forceClose(WebSocketState *state) {
    StreamWebSocket *webSocket = static_cast<StreamWebSocket *>(state);
    webSocket->fail(webSocket->failureCode);
}

bool StreamWebSocket::appendFragment(const char *data, size_t length, unsigned int remainingBytes) {
    const uint64_t required = static_cast<uint64_t>(fragmentBuffer.size()) + length + remainingBytes;
    if ((maxPayload && required > maxPayload) ||
        required > std::numeric_limits<unsigned int>::max()) {
        failureCode = 1009;
        fail(failureCode);
        return false;
    }
    if (fragmentBuffer.capacity() < required) {
        fragmentBuffer.reserve(static_cast<size_t>(required));
    }
    fragmentBuffer.append(data, length);
    return true;
}

bool StreamWebSocket::inflateMessage(const char *data, size_t length, std::string &output) {
    if (!inflaterInitialized) {
        return false;
    }

    std::string input(data, length);
    input.append(reinterpret_cast<const char *>(DEFLATE_TRAILER), sizeof(DEFLATE_TRAILER));

    inflationStream.next_in = reinterpret_cast<Bytef *>(input.data());
    inflationStream.avail_in = static_cast<uInt>(input.size());
    output.clear();

    char chunk[ZLIB_CHUNK_SIZE];
    while (inflationStream.avail_in > 0) {
        inflationStream.next_out = reinterpret_cast<Bytef *>(chunk);
        inflationStream.avail_out = sizeof(chunk);
        const uInt previousInput = inflationStream.avail_in;
        const int result = ::inflate(&inflationStream, Z_SYNC_FLUSH);
        const size_t produced = sizeof(chunk) - inflationStream.avail_out;

        if (produced) {
            if (maxPayload && output.size() + produced > maxPayload) {
                return false;
            }
            output.append(chunk, produced);
        }

        if (result != Z_OK && result != Z_BUF_ERROR && result != Z_STREAM_END) {
            return false;
        }
        if (result == Z_STREAM_END) {
            break;
        }
        if (previousInput == inflationStream.avail_in && produced == 0) {
            return false;
        }
    }

    if (extensionOptions & CLIENT_NO_CONTEXT_TAKEOVER) {
        return inflateReset(&inflationStream) == Z_OK;
    }
    return true;
}

bool StreamWebSocket::deflateMessage(const char *data, size_t length, std::string &output) {
    if (!deflaterInitialized || length > std::numeric_limits<uInt>::max()) {
        return false;
    }

    deflationStream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data));
    deflationStream.avail_in = static_cast<uInt>(length);
    output.clear();

    char chunk[ZLIB_CHUNK_SIZE];
    do {
        deflationStream.next_out = reinterpret_cast<Bytef *>(chunk);
        deflationStream.avail_out = sizeof(chunk);
        const int result = ::deflate(&deflationStream, Z_SYNC_FLUSH);
        if (result != Z_OK) {
            return false;
        }
        output.append(chunk, sizeof(chunk) - deflationStream.avail_out);
    } while (deflationStream.avail_out == 0);

    if (output.size() < sizeof(DEFLATE_TRAILER) ||
        memcmp(output.data() + output.size() - sizeof(DEFLATE_TRAILER),
               DEFLATE_TRAILER,
               sizeof(DEFLATE_TRAILER)) != 0) {
        return false;
    }
    output.resize(output.size() - sizeof(DEFLATE_TRAILER));

    if (extensionOptions & SERVER_NO_CONTEXT_TAKEOVER) {
        return deflateReset(&deflationStream) == Z_OK;
    }
    return true;
}

std::string StreamWebSocket::formatFrame(const char *data,
                                         size_t length,
                                         OpCode opCode,
                                         bool compressed) const {
    std::string frame;
    frame.resize(WebSocketProtocol::LONG_MESSAGE_HEADER + length);
    const size_t frameLength = WebSocketProtocol::formatMessage(
        frame.data(), data, length, opCode, length, compressed);
    frame.resize(frameLength);
    return frame;
}

std::string StreamWebSocket::formatClose(uint16_t code,
                                         const char *message,
                                         size_t length) const {
    constexpr size_t MAX_CLOSE_REASON = 123;
    length = std::min(length, MAX_CLOSE_REASON);
    char payload[MAX_CLOSE_REASON + 2];
    const size_t payloadLength = WebSocketProtocol::formatClosePayload(
        payload, code, message, length);
    return formatFrame(payload, payloadLength, CLOSE, false);
}

void StreamWebSocket::emitFrame(std::string frame) {
    events.push_back({StreamWebSocketEvent::Type::FRAME, std::move(frame), NONE, 0});
}

void StreamWebSocket::emitClose(uint16_t code, const char *message, size_t length) {
    events.push_back({
        StreamWebSocketEvent::Type::CLOSE,
        std::string(message ? message : "", length),
        NONE,
        code
    });
}

void StreamWebSocket::fail(uint16_t code) {
    if (closing) {
        return;
    }
    closing = true;
    if (!closeSent) {
        closeSent = true;
        emitFrame(formatClose(code, nullptr, 0));
    }
    emitClose(1006, nullptr, 0);
}

bool StreamWebSocket::handleFragment(char *data,
                                     size_t length,
                                     unsigned int remainingBytes,
                                     int opCode,
                                     bool fin,
                                     WebSocketState *state) {
    StreamWebSocket *webSocket = static_cast<StreamWebSocket *>(state);
    if (webSocket->closing) {
        return true;
    }
    if (webSocket->closeSent && opCode != CLOSE) {
        return false;
    }

    if (opCode < 3) {
        if (!remainingBytes && fin && webSocket->fragmentBuffer.empty()) {
            std::string inflated;
            bool inflatedMessage = false;
            if (webSocket->compressionStatus == CompressionStatus::COMPRESSED_FRAME) {
                webSocket->compressionStatus = CompressionStatus::ENABLED;
                if (!webSocket->inflateMessage(data, length, inflated)) {
                    webSocket->failureCode = 1007;
                    webSocket->fail(webSocket->failureCode);
                    return true;
                }
                data = inflated.data();
                length = inflated.size();
                inflatedMessage = true;
            }

            if (opCode == TEXT &&
                !WebSocketProtocol::isValidUtf8(reinterpret_cast<unsigned char *>(data), length)) {
                webSocket->failureCode = 1007;
                webSocket->fail(webSocket->failureCode);
                return true;
            }
            webSocket->events.push_back({
                StreamWebSocketEvent::Type::MESSAGE,
                inflatedMessage ? std::move(inflated) : std::string(),
                static_cast<OpCode>(opCode),
                0,
                inflatedMessage ? nullptr : data,
                inflatedMessage ? 0 : length
            });
            return false;
        }

        if (!webSocket->appendFragment(data, length, remainingBytes)) {
            return true;
        }
        if (!remainingBytes && fin) {
            std::string inflated;
            bool inflatedMessage = false;
            data = webSocket->fragmentBuffer.data();
            length = webSocket->fragmentBuffer.size();
            if (webSocket->compressionStatus == CompressionStatus::COMPRESSED_FRAME) {
                webSocket->compressionStatus = CompressionStatus::ENABLED;
                if (!webSocket->inflateMessage(data, length, inflated)) {
                    webSocket->failureCode = 1007;
                    webSocket->fail(webSocket->failureCode);
                    return true;
                }
                data = inflated.data();
                length = inflated.size();
                inflatedMessage = true;
            }

            if (opCode == TEXT &&
                !WebSocketProtocol::isValidUtf8(reinterpret_cast<unsigned char *>(data), length)) {
                webSocket->failureCode = 1007;
                webSocket->fail(webSocket->failureCode);
                return true;
            }
            webSocket->events.push_back({
                StreamWebSocketEvent::Type::MESSAGE,
                inflatedMessage ? std::move(inflated) : std::move(webSocket->fragmentBuffer),
                static_cast<OpCode>(opCode),
                0
            });
            webSocket->fragmentBuffer.clear();
            trimBuffer(webSocket->fragmentBuffer);
        }
        return false;
    }

    if (!remainingBytes && fin && !webSocket->controlTipLength) {
        if (opCode == CLOSE) {
            const WebSocketProtocol::CloseFrame closeFrame =
                WebSocketProtocol::parseClosePayload(data, length);
            if (closeFrame.code == 1006) {
                webSocket->failureCode = 1002;
                webSocket->fail(webSocket->failureCode);
                return true;
            }
            webSocket->closing = true;
            if (!webSocket->closeSent) {
                webSocket->closeSent = true;
                webSocket->emitFrame(webSocket->formatClose(
                    closeFrame.code, closeFrame.message, closeFrame.length));
            }
            webSocket->emitClose(closeFrame.code, closeFrame.message, closeFrame.length);
            return true;
        }
        if (opCode == PING) {
            webSocket->emitFrame(webSocket->formatFrame(data, length, PONG, false));
        }
        return false;
    }

    // Control frames may be interleaved with a fragmented data message and do
    // not count towards that message's payload limit. Their own wire payload
    // has already been limited to 125 bytes by WebSocketProtocol.
    webSocket->fragmentBuffer.append(data, length);
    webSocket->controlTipLength += static_cast<unsigned char>(length);

    if (!remainingBytes && fin) {
        char *controlBuffer = webSocket->fragmentBuffer.data() +
            webSocket->fragmentBuffer.size() - webSocket->controlTipLength;
        if (opCode == CLOSE) {
            const WebSocketProtocol::CloseFrame closeFrame =
                WebSocketProtocol::parseClosePayload(controlBuffer, webSocket->controlTipLength);
            if (closeFrame.code == 1006) {
                webSocket->failureCode = 1002;
                webSocket->fail(webSocket->failureCode);
                return true;
            }
            webSocket->closing = true;
            if (!webSocket->closeSent) {
                webSocket->closeSent = true;
                webSocket->emitFrame(webSocket->formatClose(
                    closeFrame.code, closeFrame.message, closeFrame.length));
            }
            webSocket->emitClose(closeFrame.code, closeFrame.message, closeFrame.length);
            return true;
        }
        if (opCode == PING) {
            webSocket->emitFrame(webSocket->formatFrame(
                controlBuffer, webSocket->controlTipLength, PONG, false));
        }
        webSocket->fragmentBuffer.resize(
            webSocket->fragmentBuffer.size() - webSocket->controlTipLength);
        webSocket->controlTipLength = 0;
        trimBuffer(webSocket->fragmentBuffer);
    }
    return false;
}

std::vector<StreamWebSocketEvent> &StreamWebSocket::consume(char *data, size_t length) {
    events.clear();
    failureCode = 1002;
    if (closing || !length) {
        return events;
    }
    if (length > std::numeric_limits<unsigned int>::max()) {
        failureCode = 1009;
        fail(failureCode);
        return events;
    }

    if (state.spillLength) {
        const size_t prePadding = WebSocketProtocol::CONSUME_PRE_PADDING;
        consumeBuffer.resize(prePadding + length);
        char *payload = consumeBuffer.data() + prePadding;
        memcpy(payload, data, length);
        WebSocketProtocol::consume(
            payload, static_cast<unsigned int>(length), this, protocolHooks);
    } else {
        WebSocketProtocol::consume(
            data, static_cast<unsigned int>(length), this, protocolHooks);
    }
    return events;
}

bool StreamWebSocket::createFrame(const char *data,
                                  size_t length,
                                  OpCode opCode,
                                  bool compress,
                                  std::string &output) {
    if (closing || closeSent || (opCode != TEXT && opCode != BINARY)) {
        return false;
    }
    if (opCode == TEXT &&
        !WebSocketProtocol::isValidUtf8(
            reinterpret_cast<unsigned char *>(const_cast<char *>(data)), length)) {
        return false;
    }

    const bool shouldCompress = compress &&
        compressionStatus != CompressionStatus::DISABLED;
    if (shouldCompress) {
        std::string deflated;
        if (!deflateMessage(data, length, deflated)) {
            return false;
        }
        output = formatFrame(deflated.data(), deflated.size(), opCode, true);
    } else {
        output = formatFrame(data, length, opCode, false);
    }
    return true;
}

bool StreamWebSocket::createCloseFrame(uint16_t code,
                                       const char *message,
                                       size_t length,
                                       std::string &output) {
    if (closing || closeSent) {
        return false;
    }
    closeSent = true;
    output = formatClose(code, message, length);
    return true;
}

} // namespace eioWS
