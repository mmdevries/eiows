#ifndef STREAMWEBSOCKET_EIOWS_H
#define STREAMWEBSOCKET_EIOWS_H

#include "Extensions.h"
#include "WebSocketProtocol.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <zlib.h>

namespace eioWS {

struct StreamWebSocketEvent {
    enum class Type : unsigned char {
        MESSAGE,
        FRAME,
        CLOSE
    };

    Type type;
    std::string data;
    OpCode opCode = NONE;
    uint16_t code = 0;
    const char *view = nullptr;
    size_t viewLength = 0;

    const char *payloadData() const { return view ? view : data.data(); }
    size_t payloadLength() const { return view ? viewLength : data.size(); }
};

enum class InflateResult : unsigned char {
    SUCCESS,
    INVALID_DATA,
    PAYLOAD_TOO_LARGE,
    INTERNAL_ERROR
};

/*
 * A server-scoped compression context. Streams are initialized on first use
 * and can be shared between sessions when no-context-takeover was negotiated.
 * Native entry points are synchronous, so a context owned by one JS Server is
 * never used concurrently.
 */
class CompressionContext {
    z_stream inflationStream = {};
    z_stream deflationStream = {};
    bool inflaterInitialized = false;
    bool deflaterInitialized = false;

    bool ensureInflater();
    bool ensureDeflater();
    bool resetInflater();
    bool resetDeflater();

public:
    CompressionContext() = default;
    ~CompressionContext();

    CompressionContext(const CompressionContext &) = delete;
    CompressionContext &operator=(const CompressionContext &) = delete;

    InflateResult inflateMessage(const char *data,
                                 size_t length,
                                 uint32_t maxPayload,
                                 bool resetAfter,
                                 std::string &output);
    bool deflateMessage(const char *data,
                        size_t length,
                        bool resetAfter,
                        std::string &output);
};

/*
 * Socket-independent WebSocket protocol state used by the owned native
 * transport after the HTTP socket has been detached from Node.js.
 */
class StreamWebSocket : public WebSocketState {
    enum class CompressionStatus : unsigned char {
        DISABLED,
        ENABLED,
        COMPRESSED_FRAME
    };

    uint32_t maxPayload;
    int extensionOptions;
    CompressionStatus compressionStatus;
    std::string fragmentBuffer;
    unsigned char controlTipLength = 0;
    std::vector<char> consumeBuffer;
    std::vector<StreamWebSocketEvent> events;
    std::shared_ptr<CompressionContext> sharedCompressionContext;
    std::unique_ptr<CompressionContext> privateCompressionContext;
    bool closing = false;
    bool closeSent = false;
    uint16_t failureCode = 1002;

    static bool refusePayloadLength(uint64_t length, WebSocketState *state);
    static bool setCompressed(WebSocketState *state);
    static void forceClose(WebSocketState *state);
    static bool handleFragment(char *data,
                               size_t length,
                               unsigned int remainingBytes,
                               int opCode,
                               bool fin,
                               WebSocketState *state);
    static const WebSocketProtocolHooks protocolHooks;

    bool appendFragment(const char *data, size_t length, unsigned int remainingBytes);
    InflateResult inflateMessage(const char *data, size_t length, std::string &output);
    bool deflateMessage(const char *data, size_t length, std::string &output);
    std::string formatFrame(const char *data, size_t length, OpCode opCode, bool compressed) const;
    std::string formatClose(uint16_t code, const char *message, size_t length) const;
    void emitFrame(std::string frame);
    void emitClose(uint16_t code, const char *message, size_t length);
    void fail(uint16_t code);

public:
    StreamWebSocket(int negotiatedOptions,
                    uint32_t maxPayload,
                    std::shared_ptr<CompressionContext> compressionContext);
    ~StreamWebSocket() = default;

    StreamWebSocket(const StreamWebSocket &) = delete;
    StreamWebSocket &operator=(const StreamWebSocket &) = delete;

    std::vector<StreamWebSocketEvent> &consume(char *data, size_t length);
    bool createFrame(const char *data,
                     size_t length,
                     OpCode opCode,
                     bool compress,
                     std::string &output);
    bool createCloseFrame(uint16_t code, const char *message, size_t length, std::string &output);
    bool isClosing() const { return closing || closeSent; }
};

} // namespace eioWS

#endif // STREAMWEBSOCKET_EIOWS_H
