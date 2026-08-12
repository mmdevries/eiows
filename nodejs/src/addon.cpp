#include <node_api.h>
#include <node_version.h>

#include "../../uWebSockets/src/Extensions.h"
#include "../../uWebSockets/src/StreamWebSocket.h"
#include "native_transport.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

const napi_type_tag SESSION_TYPE_TAG = {
    0x2c430f425d4c4f13ULL,
    0xa72e6289c930ebeaULL
};
const napi_type_tag COMPRESSION_CONTEXT_TYPE_TAG = {
    0x81191992c4fb4c82ULL,
    0xb023f25228dbf0e1ULL
};

struct SessionHandle {
    explicit SessionHandle(std::unique_ptr<eioWS::StreamWebSocket> value) :
        session(std::move(value)) {}

    std::unique_ptr<eioWS::StreamWebSocket> session;
    eiowsNode::NativeTransport *transport = nullptr;
};

struct CompressionContextHandle {
    CompressionContextHandle() :
        context(std::make_shared<eioWS::CompressionContext>()) {}

    std::shared_ptr<eioWS::CompressionContext> context;
};

struct NativeInput {
    char *data = nullptr;
    size_t length = 0;
    bool isBuffer = false;
    std::vector<char> storage;
};

bool checkStatus(napi_env env, napi_status status, const char *fallback) {
    if (status == napi_ok) {
        return true;
    }
    const napi_extended_error_info *info = nullptr;
    napi_get_last_error_info(env, &info);
    napi_throw_error(env, nullptr,
                     info && info->error_message ? info->error_message : fallback);
    return false;
}

bool getArguments(napi_env env,
                  napi_callback_info info,
                  size_t capacity,
                  std::vector<napi_value> &arguments,
                  size_t minimum = 0) {
    arguments.resize(capacity);
    size_t count = capacity;
    if (!checkStatus(env,
                     napi_get_cb_info(env, info, &count, arguments.data(), nullptr, nullptr),
                     "failed to read native arguments")) {
        return false;
    }
    if (count < (minimum ? minimum : capacity)) {
        napi_throw_type_error(env, nullptr, "not enough arguments");
        return false;
    }
    arguments.resize(count);
    return true;
}

template <size_t Capacity>
bool getFixedArguments(napi_env env,
                       napi_callback_info info,
                       std::array<napi_value, Capacity> &arguments,
                       size_t &count,
                       size_t minimum) {
    count = Capacity;
    if (!checkStatus(env,
                     napi_get_cb_info(env, info, &count, arguments.data(), nullptr, nullptr),
                     "failed to read native arguments")) {
        return false;
    }
    if (count < minimum) {
        napi_throw_type_error(env, nullptr, "not enough arguments");
        return false;
    }
    return true;
}

bool getInt32(napi_env env, napi_value value, int32_t &result) {
    return checkStatus(env, napi_get_value_int32(env, value, &result), "expected an integer");
}

bool getBoolean(napi_env env, napi_value value, bool &result) {
    return checkStatus(env, napi_get_value_bool(env, value, &result), "expected a boolean");
}

bool getString(napi_env env, napi_value value, std::string &result) {
    size_t length = 0;
    if (!checkStatus(env,
                     napi_get_value_string_utf8(env, value, nullptr, 0, &length),
                     "expected a string")) {
        return false;
    }
    std::vector<char> buffer(length + 1);
    if (!checkStatus(env,
                     napi_get_value_string_utf8(
                         env, value, buffer.data(), buffer.size(), &length),
                     "failed to read string")) {
        return false;
    }
    result.assign(buffer.data(), length);
    return true;
}

bool getInput(napi_env env, napi_value value, NativeInput &input) {
    bool isBuffer = false;
    if (!checkStatus(env, napi_is_buffer(env, value, &isBuffer), "failed to inspect Buffer")) {
        return false;
    }
    if (isBuffer) {
        void *data = nullptr;
        if (!checkStatus(env,
                         napi_get_buffer_info(env, value, &data, &input.length),
                         "failed to read Buffer")) {
            return false;
        }
        input.data = static_cast<char *>(data);
        input.isBuffer = true;
        return true;
    }

    bool isTypedArray = false;
    if (!checkStatus(env,
                     napi_is_typedarray(env, value, &isTypedArray),
                     "failed to inspect TypedArray")) {
        return false;
    }
    if (isTypedArray) {
        napi_typedarray_type type;
        size_t elementCount = 0;
        void *data = nullptr;
        napi_value arrayBuffer;
        size_t byteOffset = 0;
        if (!checkStatus(env,
                         napi_get_typedarray_info(env,
                                                  value,
                                                  &type,
                                                  &elementCount,
                                                  &data,
                                                  &arrayBuffer,
                                                  &byteOffset),
                         "failed to read TypedArray")) {
            return false;
        }
        size_t elementSize = 1;
        switch (type) {
            case napi_uint16_array:
            case napi_int16_array:
                elementSize = 2;
                break;
            case napi_uint32_array:
            case napi_int32_array:
            case napi_float32_array:
                elementSize = 4;
                break;
            case napi_float64_array:
            case napi_bigint64_array:
            case napi_biguint64_array:
                elementSize = 8;
                break;
            default:
                break;
        }
        input.data = static_cast<char *>(data);
        input.length = elementCount * elementSize;
        return true;
    }

    bool isArrayBuffer = false;
    if (!checkStatus(env,
                     napi_is_arraybuffer(env, value, &isArrayBuffer),
                     "failed to inspect ArrayBuffer")) {
        return false;
    }
    if (isArrayBuffer) {
        void *data = nullptr;
        if (!checkStatus(env,
                         napi_get_arraybuffer_info(env, value, &data, &input.length),
                         "failed to read ArrayBuffer")) {
            return false;
        }
        input.data = static_cast<char *>(data);
        return true;
    }

    napi_valuetype type;
    if (!checkStatus(env, napi_typeof(env, value, &type), "failed to inspect value")) {
        return false;
    }
    if (type == napi_string) {
        size_t length = 0;
        if (!checkStatus(env,
                         napi_get_value_string_utf8(env, value, nullptr, 0, &length),
                         "failed to read string length")) {
            return false;
        }
        input.storage.resize(length + 1);
        if (!checkStatus(env,
                         napi_get_value_string_utf8(env,
                                                    value,
                                                    input.storage.data(),
                                                    input.storage.size(),
                                                    &length),
                         "failed to read string")) {
            return false;
        }
        input.data = input.storage.data();
        input.length = length;
        return true;
    }

    napi_throw_type_error(env, nullptr, "expected a string, Buffer, TypedArray or ArrayBuffer");
    return false;
}

void finalizeSession(napi_env, void *data, void *) {
    SessionHandle *handle = static_cast<SessionHandle *>(data);
    if (handle && handle->transport) {
        eiowsNode::NativeTransport *transport = handle->transport;
        handle->transport = nullptr;
        eiowsNode::detachNativeTransportStorage(transport);
        eiowsNode::destroyNativeTransport(transport);
    }
    delete handle;
}

void finalizeCompressionContext(napi_env, void *data, void *) {
    delete static_cast<CompressionContextHandle *>(data);
}

bool hasTypeTag(napi_env env,
                napi_value value,
                const napi_type_tag *tag,
                const char *expected) {
    bool matches = false;
    if (napi_check_object_type_tag(env, value, tag, &matches) != napi_ok || !matches) {
        napi_throw_type_error(env, nullptr, expected);
        return false;
    }
    return true;
}

SessionHandle *getSession(napi_env env, napi_value value) {
    if (!hasTypeTag(env, value, &SESSION_TYPE_TAG, "expected a native WebSocket session")) {
        return nullptr;
    }
    void *data = nullptr;
    if (!checkStatus(env,
                     napi_get_value_external(env, value, &data),
                     "expected a native WebSocket session")) {
        return nullptr;
    }
    SessionHandle *handle = static_cast<SessionHandle *>(data);
    if (!handle || !handle->session) {
        napi_throw_error(env, nullptr, "WebSocket session is closed");
        return nullptr;
    }
    return handle;
}

CompressionContextHandle *getCompressionContext(napi_env env, napi_value value) {
    if (!hasTypeTag(env,
                    value,
                    &COMPRESSION_CONTEXT_TYPE_TAG,
                    "expected a native compression context")) {
        return nullptr;
    }
    void *data = nullptr;
    if (!checkStatus(env,
                     napi_get_value_external(env, value, &data),
                     "expected a native compression context")) {
        return nullptr;
    }
    CompressionContextHandle *handle = static_cast<CompressionContextHandle *>(data);
    if (!handle || !handle->context) {
        napi_throw_error(env, nullptr, "compression context is closed");
        return nullptr;
    }
    return handle;
}

napi_value createUint32(napi_env env, uint32_t value) {
    napi_value result = nullptr;
    if (!checkStatus(env,
                     napi_create_uint32(env, value, &result),
                     "failed to create integer")) {
        return nullptr;
    }
    return result;
}

napi_value createBuffer(napi_env env, const char *data, size_t length) {
    napi_value result = nullptr;
    void *copy = nullptr;
    if (!checkStatus(env,
                     napi_create_buffer_copy(env, length, data, &copy, &result),
                     "failed to create Buffer")) {
        return nullptr;
    }
    return result;
}

napi_value createBuffer(napi_env env, const std::string &data) {
    return createBuffer(env, data.data(), data.size());
}

napi_value createBufferView(napi_env env,
                            napi_value source,
                            const NativeInput &input,
                            const char *data,
                            size_t length) {
    const uintptr_t sourceAddress = reinterpret_cast<uintptr_t>(input.data);
    const uintptr_t dataAddress = reinterpret_cast<uintptr_t>(data);
    const bool isWithinSource = input.isBuffer &&
        dataAddress >= sourceAddress &&
        dataAddress - sourceAddress <= input.length &&
        length <= input.length - (dataAddress - sourceAddress);
    if (!isWithinSource) {
        return createBuffer(env, data, length);
    }
    napi_value subarray = nullptr;
    napi_value start = createUint32(env, static_cast<uint32_t>(dataAddress - sourceAddress));
    napi_value end = createUint32(
        env, static_cast<uint32_t>(dataAddress - sourceAddress + length));
    napi_value result = nullptr;
    napi_value arguments[] = {start, end};
    if (!start || !end ||
        !checkStatus(env,
                     napi_get_named_property(env, source, "subarray", &subarray),
                     "failed to access Buffer.subarray") ||
        !checkStatus(env,
                     napi_call_function(env, source, subarray, 2, arguments, &result),
                     "failed to create Buffer view")) {
        return nullptr;
    }
    return result;
}

void finalizeBuffer(napi_env, void *, void *hint) {
    delete static_cast<std::string *>(hint);
}

napi_value createExternalBuffer(napi_env env, std::string &&data) {
    if (data.empty()) {
        return createBuffer(env, data);
    }
    auto *owned = new std::string(std::move(data));
    napi_value result = nullptr;
    const napi_status status = napi_create_external_buffer(
        env, owned->size(), owned->data(), finalizeBuffer, owned, &result);
    if (status == napi_no_external_buffers_allowed) {
        result = createBuffer(env, *owned);
        delete owned;
        return result;
    }
    if (!checkStatus(env, status, "failed to create external Buffer")) {
        delete owned;
        return nullptr;
    }
    return result;
}

napi_value createString(napi_env env, const char *data, size_t length) {
    napi_value result = nullptr;
    if (!checkStatus(env,
                     napi_create_string_utf8(env, data, length, &result),
                     "failed to create string")) {
        return nullptr;
    }
    return result;
}

napi_value createString(napi_env env, const std::string &data) {
    return createString(env, data.data(), data.size());
}

napi_value createCompressionContext(napi_env env, napi_callback_info) {
    auto *handle = new CompressionContextHandle();
    napi_value external = nullptr;
    if (!checkStatus(env,
                     napi_create_external(
                         env, handle, finalizeCompressionContext, nullptr, &external),
                     "failed to create compression context")) {
        delete handle;
        return nullptr;
    }
    if (!checkStatus(env,
                     napi_type_tag_object(env, external, &COMPRESSION_CONTEXT_TYPE_TAG),
                     "failed to tag compression context")) {
        return nullptr;
    }
    return external;
}

napi_value createSession(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 4, args, 3)) {
        return nullptr;
    }

    int32_t wantedOptions = 0;
    int32_t maxPayload = 0;
    std::string extensionOffer;
    if (!getInt32(env, args[0], wantedOptions) ||
        !getInt32(env, args[1], maxPayload) ||
        !getString(env, args[2], extensionOffer)) {
        return nullptr;
    }
    if (maxPayload < 0) {
        napi_throw_range_error(env, nullptr, "maxPayload must be a non-negative 32-bit integer");
        return nullptr;
    }

    eioWS::ExtensionsNegotiator negotiator(wantedOptions);
    negotiator.readOffer(extensionOffer);
    const int negotiatedOptions = negotiator.getNegotiatedOptions();

    std::shared_ptr<eioWS::CompressionContext> compressionContext;
    if (args.size() == 4) {
        napi_valuetype type;
        if (!checkStatus(env, napi_typeof(env, args[3], &type), "failed to inspect compression context")) {
            return nullptr;
        }
        if (type != napi_undefined && type != napi_null) {
            CompressionContextHandle *contextHandle = getCompressionContext(env, args[3]);
            if (!contextHandle) {
                return nullptr;
            }
            compressionContext = contextHandle->context;
        }
    }
    if (!compressionContext && (negotiatedOptions & eioWS::PERMESSAGE_DEFLATE)) {
        compressionContext = std::make_shared<eioWS::CompressionContext>();
    }

    auto session = std::make_unique<eioWS::StreamWebSocket>(
        negotiatedOptions, static_cast<uint32_t>(maxPayload), compressionContext);
    auto *handle = new SessionHandle(std::move(session));

    napi_value external = nullptr;
    if (!checkStatus(env,
                     napi_create_external(
                         env, handle, finalizeSession, nullptr, &external),
                     "failed to create WebSocket session")) {
        delete handle;
        return nullptr;
    }
    if (!checkStatus(env,
                     napi_type_tag_object(env, external, &SESSION_TYPE_TAG),
                     "failed to tag WebSocket session")) {
        return nullptr;
    }

    napi_value result = nullptr;
    napi_value extensionResponse = createString(env, negotiator.generateOffer());
    if (!extensionResponse ||
        !checkStatus(env, napi_create_array_with_length(env, 2, &result), "failed to create result") ||
        !checkStatus(env, napi_set_element(env, result, 0, external), "failed to set session") ||
        !checkStatus(env, napi_set_element(env, result, 1, extensionResponse), "failed to set extensions")) {
        return nullptr;
    }
    return result;
}

napi_value consume(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 3, args, 2)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    NativeInput input;
    bool textAsBuffer = false;
    if (!handle || !getInput(env, args[1], input) ||
        (args.size() == 3 && !getBoolean(env, args[2], textAsBuffer))) {
        return nullptr;
    }

    std::vector<eioWS::StreamWebSocketEvent> &events =
        handle->session->consume(input.data, input.length);
    struct ClearEventsOnReturn {
        std::vector<eioWS::StreamWebSocketEvent> &events;
        ~ClearEventsOnReturn() { events.clear(); }
    } clearEventsOnReturn{events};

    napi_value result = nullptr;
    if (!checkStatus(env,
                     napi_create_array_with_length(env, events.size(), &result),
                     "failed to create event list")) {
        return nullptr;
    }

    for (size_t index = 0; index < events.size(); index++) {
        eioWS::StreamWebSocketEvent &event = events[index];
        const uint32_t eventLength = event.type == eioWS::StreamWebSocketEvent::Type::MESSAGE ? 3 :
            event.type == eioWS::StreamWebSocketEvent::Type::CLOSE ? 3 : 2;
        napi_value item = nullptr;
        if (!checkStatus(env,
                         napi_create_array_with_length(env, eventLength, &item),
                         "failed to create event")) {
            return nullptr;
        }

        const uint32_t eventType = event.type == eioWS::StreamWebSocketEvent::Type::MESSAGE ? 0 :
            event.type == eioWS::StreamWebSocketEvent::Type::FRAME ? 1 : 2;
        napi_value typeValue = createUint32(env, eventType);
        if (!typeValue ||
            !checkStatus(env, napi_set_element(env, item, 0, typeValue), "failed to set event type")) {
            return nullptr;
        }

        if (event.type == eioWS::StreamWebSocketEvent::Type::MESSAGE) {
            napi_value message = event.opCode == eioWS::BINARY || textAsBuffer
                ? createBufferView(
                    env, args[1], input, event.payloadData(), event.payloadLength())
                : createString(env, event.payloadData(), event.payloadLength());
            napi_value binary = nullptr;
            if (!message ||
                !checkStatus(env,
                             napi_get_boolean(env, event.opCode == eioWS::BINARY, &binary),
                             "failed to create binary flag") ||
                !checkStatus(env, napi_set_element(env, item, 1, message), "failed to set message") ||
                !checkStatus(env, napi_set_element(env, item, 2, binary), "failed to set binary flag")) {
                return nullptr;
            }
        } else if (event.type == eioWS::StreamWebSocketEvent::Type::FRAME) {
            napi_value frame = createExternalBuffer(env, std::move(event.data));
            if (!frame ||
                !checkStatus(env, napi_set_element(env, item, 1, frame), "failed to set frame")) {
                return nullptr;
            }
        } else {
            napi_value code = createUint32(env, event.code);
            napi_value reason = createString(env, event.payloadData(), event.payloadLength());
            if (!code || !reason ||
                !checkStatus(env, napi_set_element(env, item, 1, code), "failed to set close code") ||
                !checkStatus(env, napi_set_element(env, item, 2, reason), "failed to set close reason")) {
                return nullptr;
            }
        }

        if (!checkStatus(env,
                         napi_set_element(env, result, index, item),
                         "failed to append event")) {
            return nullptr;
        }
    }
    // All JS values now own or reference their payload. ClearEventsOnReturn
    // releases native event storage immediately, including on N-API errors.
    return result;
}

napi_value createFrame(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 4, args)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    NativeInput input;
    int32_t opCode = 0;
    bool compress = false;
    if (!handle ||
        !getInput(env, args[1], input) ||
        !getInt32(env, args[2], opCode) ||
        !getBoolean(env, args[3], compress)) {
        return nullptr;
    }

    std::string frame;
    if (!handle->session->createFrame(
            input.data, input.length, static_cast<eioWS::OpCode>(opCode), compress, frame)) {
        napi_throw_error(env, nullptr, "cannot send WebSocket frame in the current state");
        return nullptr;
    }
    return createExternalBuffer(env, std::move(frame));
}

napi_value createCloseFrame(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 3, args)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    int32_t code = 0;
    NativeInput reason;
    if (!handle || !getInt32(env, args[1], code) || !getInput(env, args[2], reason)) {
        return nullptr;
    }

    std::string frame;
    if (!handle->session->createCloseFrame(
            static_cast<uint16_t>(code), reason.data, reason.length, frame)) {
        napi_value nullValue = nullptr;
        if (!checkStatus(env, napi_get_null(env, &nullValue), "failed to create null")) {
            return nullptr;
        }
        return nullValue;
    }
    return createExternalBuffer(env, std::move(frame));
}

napi_value attachTransport(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 6, args)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    bool textAsBuffer = false;
    bool encrypted = false;
    int32_t maxBackpressure = 0;
    if (!handle || !getBoolean(env, args[3], textAsBuffer) ||
        !getBoolean(env, args[4], encrypted) ||
        !getInt32(env, args[5], maxBackpressure)) {
        return nullptr;
    }
    if (maxBackpressure < 0) {
        napi_throw_range_error(
            env, nullptr, "maxBackpressure must be a non-negative 32-bit integer");
        return nullptr;
    }
    if (handle->transport) {
        napi_throw_error(env, nullptr, "native transport is already attached");
        return nullptr;
    }

    handle->transport = eiowsNode::attachNativeTransport(
        env,
        handle->session.get(),
        args[1],
        args[2],
        textAsBuffer,
        encrypted,
        static_cast<size_t>(maxBackpressure),
        &handle->transport);
    napi_value result = nullptr;
    if (!checkStatus(env, napi_get_boolean(env, handle->transport != nullptr, &result),
                     "failed to create native transport result")) {
        return nullptr;
    }
    return result;
}

napi_value feedTransport(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 2, args)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    NativeInput input;
    if (!handle || !getInput(env, args[1], input)) {
        return nullptr;
    }
    if (!handle->transport) {
        napi_throw_error(env, nullptr, "native transport is not attached");
        return nullptr;
    }
    const bool consumed = eiowsNode::feedNativeTransport(
        handle->transport, args[1], input.data, input.length);
    napi_value result = nullptr;
    if (!checkStatus(env, napi_get_boolean(env, consumed, &result),
                     "failed to create native feed result")) {
        return nullptr;
    }
    return result;
}

napi_value activateTransport(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 1, args)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    if (!handle) return nullptr;
    if (!handle->transport) {
        napi_throw_error(env, nullptr, "native transport is not attached");
        return nullptr;
    }
    const int status = eiowsNode::activateNativeTransport(handle->transport);
    napi_value result = nullptr;
    if (!checkStatus(env, napi_create_int32(env, status, &result),
                     "failed to create native activation status")) {
        return nullptr;
    }
    return result;
}

napi_value terminateTransport(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 1, args)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    if (!handle) return nullptr;
    eiowsNode::terminateNativeTransport(handle->transport);
    napi_value undefined = nullptr;
    if (!checkStatus(env, napi_get_undefined(env, &undefined),
                     "failed to create undefined")) {
        return nullptr;
    }
    return undefined;
}

napi_value writeTransportMessage(napi_env env, napi_callback_info info) {
    std::array<napi_value, 5> args{};
    size_t count = 0;
    if (!getFixedArguments(env, info, args, count, 4)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    int32_t opCode = 0;
    bool compress = false;
    if (!handle || !getInt32(env, args[2], opCode) || !getBoolean(env, args[3], compress)) {
        return nullptr;
    }
    if (!handle->transport) {
        napi_throw_error(env, nullptr, "native transport is not attached");
        return nullptr;
    }
    napi_value callback = count == 5 ? args[4] : nullptr;
    const int status = eiowsNode::writeNativeMessage(
        handle->transport,
        args[1],
        static_cast<eioWS::OpCode>(opCode),
        compress,
        callback);
    napi_value result = nullptr;
    if (!checkStatus(env, napi_create_int32(env, status, &result),
                     "failed to create native write status")) {
        return nullptr;
    }
    return result;
}

napi_value writeTransportFrames(napi_env env, napi_callback_info info) {
    std::array<napi_value, 3> args{};
    size_t count = 0;
    if (!getFixedArguments(env, info, args, count, 2)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    if (!handle) return nullptr;
    if (!handle->transport) {
        napi_throw_error(env, nullptr, "native transport is not attached");
        return nullptr;
    }
    napi_value callback = count == 3 ? args[2] : nullptr;
    const int status = eiowsNode::writeNativeFrameList(
        handle->transport, args[1], callback);
    napi_value result = nullptr;
    if (!checkStatus(env, napi_create_int32(env, status, &result),
                     "failed to create native frame write status")) {
        return nullptr;
    }
    return result;
}

napi_value writeTransportFrame(napi_env env, napi_callback_info info) {
    std::array<napi_value, 3> args{};
    size_t count = 0;
    if (!getFixedArguments(env, info, args, count, 2)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    if (!handle) return nullptr;
    if (!handle->transport) {
        napi_throw_error(env, nullptr, "native transport is not attached");
        return nullptr;
    }
    napi_value callback = count == 3 ? args[2] : nullptr;
    const int status = eiowsNode::writeNativeFrame(
        handle->transport, args[1], callback);
    napi_value result = nullptr;
    if (!checkStatus(env, napi_create_int32(env, status, &result),
                     "failed to create native write status")) {
        return nullptr;
    }
    return result;
}

napi_value writeTransportClose(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 3, args)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    int32_t code = 0;
    NativeInput reason;
    if (!handle || !getInt32(env, args[1], code) || !getInput(env, args[2], reason)) {
        return nullptr;
    }
    if (!handle->transport) {
        napi_throw_error(env, nullptr, "native transport is not attached");
        return nullptr;
    }
    const int status = eiowsNode::writeNativeClose(
        handle->transport,
        static_cast<uint16_t>(code),
        reason.data,
        reason.length);
    napi_value result = nullptr;
    if (!checkStatus(env, napi_create_int32(env, status, &result),
                     "failed to create native close write status")) {
        return nullptr;
    }
    return result;
}

napi_value transportBufferedAmount(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 1, args)) {
        return nullptr;
    }
    SessionHandle *handle = getSession(env, args[0]);
    if (!handle) return nullptr;
    napi_value result = nullptr;
    if (!checkStatus(env,
                     napi_create_double(
                         env,
                         static_cast<double>(eiowsNode::nativeBufferedAmount(handle->transport)),
                         &result),
                     "failed to create native buffered amount")) {
        return nullptr;
    }
    return result;
}

napi_value dispose(napi_env env, napi_callback_info info) {
    std::vector<napi_value> args;
    if (!getArguments(env, info, 1, args)) {
        return nullptr;
    }
    if (!hasTypeTag(env,
                    args[0],
                    &SESSION_TYPE_TAG,
                    "expected a native WebSocket session")) {
        return nullptr;
    }
    void *data = nullptr;
    if (!checkStatus(env,
                     napi_get_value_external(env, args[0], &data),
                     "expected a native WebSocket session")) {
        return nullptr;
    }
    SessionHandle *handle = static_cast<SessionHandle *>(data);
    if (handle) {
        if (handle->transport) {
            eiowsNode::NativeTransport *transport = handle->transport;
            handle->transport = nullptr;
            eiowsNode::detachNativeTransportStorage(transport);
            eiowsNode::destroyNativeTransport(transport);
        }
        handle->session.reset();
    }
    napi_value undefined = nullptr;
    if (!checkStatus(env, napi_get_undefined(env, &undefined), "failed to create undefined")) {
        return nullptr;
    }
    return undefined;
}

template <napi_value (*Callback)(napi_env, napi_callback_info)>
napi_value guardedCallback(napi_env env, napi_callback_info info) {
    try {
        return Callback(env, info);
    } catch (const std::bad_alloc &) {
        napi_throw_error(env, nullptr, "native WebSocket allocation failed");
    } catch (const std::exception &error) {
        napi_throw_error(env, nullptr, error.what());
    } catch (...) {
        napi_throw_error(env, nullptr, "unknown native WebSocket failure");
    }
    return nullptr;
}

napi_value initialize(napi_env env, napi_value exports) {
    const napi_node_version *runtimeVersion = nullptr;
    if (napi_get_node_version(env, &runtimeVersion) != napi_ok || !runtimeVersion) {
        napi_throw_error(env, nullptr, "failed to read the Node.js runtime version");
        return nullptr;
    }
    if (runtimeVersion->major != NODE_MAJOR_VERSION ||
        runtimeVersion->minor != NODE_MINOR_VERSION ||
        runtimeVersion->patch != NODE_PATCH_VERSION) {
        char message[192];
        std::snprintf(
            message,
            sizeof(message),
            "eiows was built for Node.js %d.%d.%d but is running on Node.js %u.%u.%u; "
            "rebuild eiows for the current Node.js release",
            NODE_MAJOR_VERSION,
            NODE_MINOR_VERSION,
            NODE_PATCH_VERSION,
            runtimeVersion->major,
            runtimeVersion->minor,
            runtimeVersion->patch);
        napi_throw_error(env, nullptr, message);
        return nullptr;
    }
    if (!eiowsNode::initializeNativeEnvironment(env)) {
        napi_throw_error(env, nullptr, "failed to initialize native transport environment");
        return nullptr;
    }
    const napi_property_descriptor properties[] = {
        {"createCompressionContext", nullptr, guardedCallback<createCompressionContext>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createSession", nullptr, guardedCallback<createSession>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"consume", nullptr, guardedCallback<consume>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"frame", nullptr, guardedCallback<createFrame>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"closeFrame", nullptr, guardedCallback<createCloseFrame>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"attachTransport", nullptr, guardedCallback<attachTransport>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"activateTransport", nullptr, guardedCallback<activateTransport>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"terminateTransport", nullptr, guardedCallback<terminateTransport>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"feedTransport", nullptr, guardedCallback<feedTransport>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"writeTransportMessage", nullptr, guardedCallback<writeTransportMessage>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"writeTransportFrame", nullptr, guardedCallback<writeTransportFrame>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"writeTransportFrames", nullptr, guardedCallback<writeTransportFrames>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"writeTransportClose", nullptr, guardedCallback<writeTransportClose>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"transportBufferedAmount", nullptr, guardedCallback<transportBufferedAmount>, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"dispose", nullptr, guardedCallback<dispose>, nullptr, nullptr, nullptr, napi_default, nullptr}
    };
    if (!checkStatus(env,
                     napi_define_properties(
                         env, exports, sizeof(properties) / sizeof(properties[0]), properties),
                     "failed to initialize eiows")) {
        return nullptr;
    }
    return exports;
}

} // namespace

NAPI_MODULE(NODE_GYP_MODULE_NAME, initialize)
