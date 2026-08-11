#include "native_transport.h"

#include <async_wrap-inl.h>
#include <crypto/crypto_tls.h>
#include <js_native_api_v8.h>
#include <node.h>
#include <node_buffer.h>
#include <stream_base-inl.h>
#include <stream_base.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <uv.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace eiowsNode {

namespace {

constexpr int WRITE_SYNCHRONOUS = 0;
constexpr int WRITE_ASYNCHRONOUS = 1;
constexpr int WRITE_NOT_SENT = 2;
constexpr size_t SHARED_READ_BUFFER_SIZE = 64 * 1024;
// Avoid encrypting the 2-14 byte WebSocket header as its own TLS record. Only
// the first 4 KiB is coalesced; the remainder can keep referencing a shared
// broadcast payload instead of being copied per connection under backpressure.
constexpr size_t TLS_COALESCE_PREFIX = 4 * 1024;
constexpr size_t TLS_FULL_COALESCE_LIMIT = 16 * 1024 + 14;
constexpr size_t TLS_OUTPUT_BATCH_SIZE = 64 * 1024;
// Feed several TLS records to the memory BIO before flushing them to the
// descriptor. Keeping plaintext below the ciphertext scratch limit leaves
// room for record headers, authentication tags and legacy cipher padding.
constexpr size_t TLS_PLAINTEXT_BATCH_SIZE = 60 * 1024;

thread_local std::array<char, SHARED_READ_BUFFER_SIZE> sharedReadBuffer;
thread_local std::vector<char> sharedTLSOutputBuffer;

size_t maximumIOVectors() {
    static const size_t limit = [] {
        const long configured = sysconf(_SC_IOV_MAX);
        return configured > 0 ? static_cast<size_t>(configured) : size_t{16};
    }();
    return limit;
}

template <typename Tag, typename Tag::type Member>
struct PrivateMemberAccess {
    friend typename Tag::type getPrivateMember(Tag) { return Member; }
};

struct TLSWrapSSLMember {
    using type = ncrypto::SSLPointer node::crypto::TLSWrap::*;
    friend type getPrivateMember(TLSWrapSSLMember);
};

struct TLSWrapContextMember {
    using type = node::BaseObjectPtr<node::crypto::SecureContext>
        node::crypto::TLSWrap::*;
    friend type getPrivateMember(TLSWrapContextMember);
};

template struct PrivateMemberAccess<
    TLSWrapSSLMember,
    &node::crypto::TLSWrap::ssl_>;
template struct PrivateMemberAccess<
    TLSWrapContextMember,
    &node::crypto::TLSWrap::sc_>;

SSL *getTLSWrapSSL(node::crypto::TLSWrap *wrap) {
    return (wrap->*getPrivateMember(TLSWrapSSLMember{})).get();
}

SSL_CTX *getTLSWrapInitialContext(node::crypto::TLSWrap *wrap) {
    auto &context = wrap->*getPrivateMember(TLSWrapContextMember{});
    return context ? context->ctx().get() : nullptr;
}

v8::Local<v8::Value> localValue(napi_value value) {
    return v8impl::V8LocalValueFromJsValue(value);
}

bool checkStatus(napi_env env, napi_status status, const char *message) {
    if (status == napi_ok) return true;
    if (status != napi_pending_exception) napi_throw_error(env, nullptr, message);
    return false;
}

napi_value createBufferCopy(const char *data, size_t length) {
    static const char empty = 0;
    if (!data) data = &empty;
    v8::Local<v8::Object> result;
    if (!node::Buffer::Copy(v8::Isolate::GetCurrent(), data, length).ToLocal(&result)) {
        return nullptr;
    }
    return v8impl::JsValueFromV8LocalValue(result);
}

napi_value createBufferSlice(napi_value source,
                             const char *sourceData,
                             size_t sourceLength,
                             const char *data,
                             size_t length) {
    const uintptr_t sourceAddress = reinterpret_cast<uintptr_t>(sourceData);
    const uintptr_t dataAddress = reinterpret_cast<uintptr_t>(data);
    if (!source || !sourceData || dataAddress < sourceAddress ||
        dataAddress - sourceAddress > sourceLength ||
        length > sourceLength - (dataAddress - sourceAddress)) {
        return createBufferCopy(data, length);
    }
    v8::Local<v8::Value> sourceValue = localValue(source);
    if (!node::Buffer::HasInstance(sourceValue)) {
        return createBufferCopy(data, length);
    }
    v8::Local<v8::Uint8Array> sourceBuffer = sourceValue.As<v8::Uint8Array>();
    v8::Local<v8::Uint8Array> slice;
    const size_t offset = sourceBuffer->ByteOffset() +
        static_cast<size_t>(dataAddress - sourceAddress);
    if (!node::Buffer::New(
            v8::Isolate::GetCurrent(), sourceBuffer->Buffer(), offset, length).ToLocal(&slice)) {
        return nullptr;
    }
    return v8impl::JsValueFromV8LocalValue(slice);
}

struct OwnedBuffer {
    explicit OwnedBuffer(std::string value) : data(std::move(value)) {}
    std::string data;
};

void finalizeOwnedBuffer(napi_env, void *, void *hint) {
    delete static_cast<OwnedBuffer *>(hint);
}

napi_value createOwnedBuffer(napi_env env,
                             std::string value) {
    if (value.empty()) return createBufferCopy(nullptr, 0);
    auto *owned = new OwnedBuffer(std::move(value));
    napi_value result = nullptr;
    const napi_status status = napi_create_external_buffer(
        env,
        owned->data.size(),
        owned->data.data(),
        finalizeOwnedBuffer,
        owned,
        &result);
    if (status == napi_no_external_buffers_allowed) {
        result = createBufferCopy(owned->data.data(), owned->data.size());
        delete owned;
        return result;
    }
    if (!checkStatus(env, status, "failed to create native message Buffer")) {
        delete owned;
        return nullptr;
    }
    return result;
}

napi_value createString(napi_env env, const char *data, size_t length) {
    if (length > static_cast<size_t>(std::numeric_limits<int>::max())) {
        napi_throw_range_error(env, nullptr, "native WebSocket string is too large");
        return nullptr;
    }
    v8::Local<v8::String> result;
    if (!v8::String::NewFromUtf8(
            v8::Isolate::GetCurrent(),
            data,
            v8::NewStringType::kNormal,
            static_cast<int>(length)).ToLocal(&result)) {
        return nullptr;
    }
    return v8impl::JsValueFromV8LocalValue(result);
}

class InputPart {
public:
    InputPart() = default;
    InputPart(const InputPart &) = delete;
    InputPart &operator=(const InputPart &) = delete;

    InputPart(InputPart &&other) noexcept :
        backingStore(std::move(other.backingStore)),
        owned(std::move(other.owned)),
        source(other.source),
        pointer(other.pointer),
        length(other.length),
        ownedOffset(other.ownedOffset) {
        other.pointer = nullptr;
        other.source = nullptr;
        other.length = 0;
        other.ownedOffset = 0;
    }

    InputPart &operator=(InputPart &&other) noexcept {
        if (this == &other) return *this;
        if (backingStore) std::abort();
        backingStore = std::move(other.backingStore);
        owned = std::move(other.owned);
        source = other.source;
        pointer = other.pointer;
        length = other.length;
        ownedOffset = other.ownedOffset;
        other.pointer = nullptr;
        other.source = nullptr;
        other.length = 0;
        other.ownedOffset = 0;
        return *this;
    }

    void release(napi_env) {
        backingStore.reset();
        owned.clear();
        source = nullptr;
        pointer = nullptr;
        length = 0;
        ownedOffset = 0;
    }

    std::shared_ptr<v8::BackingStore> backingStore;
    std::string owned;
    // Valid only until submit() returns; defer() converts it to a backingStore.
    napi_value source = nullptr;
    char *pointer = nullptr;
    size_t length = 0;
    size_t ownedOffset = 0;
};

void retainBackingStore(napi_value value, InputPart &part) {
    v8::Local<v8::Value> input = localValue(value);
    if (input->IsArrayBufferView()) {
        part.backingStore =
            input.As<v8::ArrayBufferView>()->Buffer()->GetBackingStore();
    } else if (input->IsArrayBuffer()) {
        part.backingStore = input.As<v8::ArrayBuffer>()->GetBackingStore();
    }
    part.source = nullptr;
}

bool readStringInputPart(napi_env env, napi_value value, InputPart &part) {
    size_t length = 0;
    if (!checkStatus(env, napi_get_value_string_utf8(env, value, nullptr, 0, &length),
                     "failed to read string length")) {
        return false;
    }
    part.owned.resize(length + 1);
    size_t written = 0;
    if (!checkStatus(env,
                     napi_get_value_string_utf8(
                         env, value, part.owned.data(), part.owned.size(), &written),
                     "failed to encode string")) {
        return false;
    }
    part.owned.resize(written);
    part.pointer = part.owned.data();
    part.length = part.owned.size();
    return true;
}

bool readInputPart(napi_env env, napi_value value, InputPart &part) {
    bool isBuffer = false;
    if (!checkStatus(env, napi_is_buffer(env, value, &isBuffer),
                     "failed to inspect Buffer")) {
        return false;
    }
    if (isBuffer) {
        void *data = nullptr;
        if (!checkStatus(env, napi_get_buffer_info(env, value, &data, &part.length),
                         "failed to read Buffer")) {
            return false;
        }
        part.pointer = static_cast<char *>(data);
        part.source = value;
        return true;
    }

    bool isTypedArray = false;
    if (!checkStatus(env, napi_is_typedarray(env, value, &isTypedArray),
                     "failed to inspect TypedArray")) {
        return false;
    }
    if (isTypedArray) {
        napi_typedarray_type type;
        size_t elementCount = 0;
        void *data = nullptr;
        napi_value arrayBuffer = nullptr;
        size_t byteOffset = 0;
        if (!checkStatus(env,
                         napi_get_typedarray_info(env, value, &type, &elementCount, &data,
                                                  &arrayBuffer, &byteOffset),
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
        if (elementCount > std::numeric_limits<size_t>::max() / elementSize) {
            napi_throw_range_error(env, nullptr, "TypedArray is too large");
            return false;
        }
        part.pointer = static_cast<char *>(data);
        part.length = elementCount * elementSize;
        part.source = value;
        return true;
    }

    bool isDataView = false;
    if (!checkStatus(env, napi_is_dataview(env, value, &isDataView),
                     "failed to inspect DataView")) {
        return false;
    }
    if (isDataView) {
        void *data = nullptr;
        napi_value arrayBuffer = nullptr;
        size_t byteOffset = 0;
        if (!checkStatus(env,
                         napi_get_dataview_info(
                             env, value, &part.length, &data, &arrayBuffer, &byteOffset),
                         "failed to read DataView")) {
            return false;
        }
        part.pointer = static_cast<char *>(data);
        part.source = value;
        return true;
    }

    bool isArrayBuffer = false;
    if (!checkStatus(env, napi_is_arraybuffer(env, value, &isArrayBuffer),
                     "failed to inspect ArrayBuffer")) {
        return false;
    }
    if (isArrayBuffer) {
        void *data = nullptr;
        if (!checkStatus(env, napi_get_arraybuffer_info(env, value, &data, &part.length),
                         "failed to read ArrayBuffer")) {
            return false;
        }
        part.pointer = static_cast<char *>(data);
        part.source = value;
        return true;
    }

    napi_valuetype type;
    if (!checkStatus(env, napi_typeof(env, value, &type),
                     "failed to inspect write value")) {
        return false;
    }
    if (type != napi_string) {
        napi_throw_type_error(
            env, nullptr, "expected a string, Buffer, TypedArray or ArrayBuffer");
        return false;
    }
    return readStringInputPart(env, value, part);
}

std::string frameHeader(size_t payloadLength, eioWS::OpCode opCode) {
    std::string header;
    const unsigned char first = 0x80 | static_cast<unsigned char>(opCode);
    if (payloadLength < 126) {
        header.resize(2);
        header[0] = static_cast<char>(first);
        header[1] = static_cast<char>(payloadLength);
    } else if (payloadLength <= 0xffff) {
        header.resize(4);
        header[0] = static_cast<char>(first);
        header[1] = 126;
        header[2] = static_cast<char>((payloadLength >> 8) & 0xff);
        header[3] = static_cast<char>(payloadLength & 0xff);
    } else {
        header.resize(10);
        header[0] = static_cast<char>(first);
        header[1] = 127;
        const uint64_t length = payloadLength;
        for (unsigned int index = 0; index < 8; index++) {
            header[2 + index] = static_cast<char>(length >> ((7 - index) * 8));
        }
    }
    return header;
}

struct WriteRequest {
private:
    struct FreeNode { FreeNode *next; };
    struct Cache {
        FreeNode *head = nullptr;
        size_t count = 0;
        ~Cache();
    };
    static constexpr size_t MAX_CACHED_REQUESTS = 64;
    static thread_local Cache cache_;

public:
    static constexpr size_t INLINE_PARTS = 2;
    static constexpr size_t INLINE_BYTES = 1024 + 14;

    explicit WriteRequest(napi_env value) : env(value) {}

    static void *operator new(size_t size) {
        if (size == sizeof(WriteRequest) && cache_.head) {
            FreeNode *node = cache_.head;
            cache_.head = node->next;
            cache_.count--;
            return node;
        }
        return ::operator new(size);
    }

    static void operator delete(void *memory) noexcept {
        if (!memory) return;
        if (cache_.count >= MAX_CACHED_REQUESTS) {
            ::operator delete(memory);
            return;
        }
        auto *node = new (memory) FreeNode{cache_.head};
        cache_.head = node;
        cache_.count++;
    }

    ~WriteRequest() {
        clearParts();
        if (callback) napi_delete_reference(env, callback);
    }

    void clearParts() {
        for (size_t index = 0; index < partCount; index++) part(index).release(env);
        extraParts.clear();
        iovecs.clear();
        partCount = 0;
        cursorPart = 0;
        cursorOffset = 0;
    }

    void abandonReferences() {
        callback = nullptr;
        callbackValue = nullptr;
    }

    bool retainCallback(napi_value value) {
        if (!value) return true;
        napi_valuetype type;
        if (!checkStatus(env, napi_typeof(env, value, &type),
                         "failed to inspect callback")) {
            return false;
        }
        if (type == napi_undefined || type == napi_null) return true;
        if (type != napi_function) {
            napi_throw_type_error(env, nullptr, "write callback must be a function");
            return false;
        }
        callbackValue = value;
        return true;
    }

    bool defer() {
        if (deferred) return true;
        retainRemainingInputs();
        if (callbackValue &&
            !checkStatus(env,
                         napi_create_reference(env, callbackValue, 1, &callback),
                         "failed to retain write callback")) {
            callbackValue = nullptr;
            return false;
        }
        callbackValue = nullptr;
        deferred = true;
        return true;
    }

    void addOwned(std::string value) {
        InputPart &target = appendPart();
        target.owned = std::move(value);
        target.pointer = target.owned.data();
        target.length = target.owned.size();
    }

    bool addInput(napi_value value) {
        InputPart &target = appendPart();
        if (readInputPart(env, value, target)) return true;
        target.release(env);
        removeLastPart();
        return false;
    }

    bool addTextInput(napi_value value) {
        InputPart &target = appendPart();
        if (readStringInputPart(env, value, target)) return true;
        target.release(env);
        removeLastPart();
        return false;
    }

    void prependOwned(std::string value) {
        if (partCount == 0) {
            addOwned(std::move(value));
            return;
        }
        if (partCount != 1) std::abort();
        inlineParts[1] = std::move(inlineParts[0]);
        partCount = 2;
        inlineParts[0].owned = std::move(value);
        inlineParts[0].pointer = inlineParts[0].owned.data();
        inlineParts[0].length = inlineParts[0].owned.size();
    }

    bool prepare() {
        bytes = 0;
        cursorPart = 0;
        cursorOffset = 0;
        for (size_t index = 0; index < partCount; index++) {
            InputPart &current = part(index);
            if (!current.owned.empty()) {
                current.pointer = current.owned.data() + current.ownedOffset;
            }
            if (bytes > std::numeric_limits<size_t>::max() - current.length) {
                napi_throw_range_error(env, nullptr, "native write is too large");
                return false;
            }
            bytes += current.length;
        }
        skipEmptyParts();
        return partCount != 0;
    }

    bool flatten() {
        if (partCount <= 1) return true;
        if (bytes <= inlineBytes.size()) {
            size_t offset = 0;
            for (size_t index = 0; index < partCount; index++) {
                InputPart &current = part(index);
                if (current.length) {
                    std::memcpy(inlineBytes.data() + offset,
                                current.pointer,
                                current.length);
                    offset += current.length;
                }
            }
            clearParts();
            InputPart &combined = appendPart();
            combined.pointer = inlineBytes.data();
            combined.length = offset;
            return prepare();
        }
        std::string combined;
        combined.resize(bytes);
        size_t offset = 0;
        for (size_t index = 0; index < partCount; index++) {
            InputPart &current = part(index);
            if (current.length) {
                std::memcpy(combined.data() + offset, current.pointer, current.length);
                offset += current.length;
            }
        }
        clearParts();
        addOwned(std::move(combined));
        return prepare();
    }

    bool coalescePrefix(size_t prefixLength) {
        if (partCount <= 1 || prefixLength >= bytes) return flatten();
        std::string prefix(prefixLength, '\0');
        size_t copied = 0;
        for (size_t index = 0; index < partCount && copied < prefixLength; index++) {
            InputPart &current = part(index);
            const size_t length = std::min(current.length, prefixLength - copied);
            if (length) std::memcpy(prefix.data() + copied, current.pointer, length);
            copied += length;
        }
        if (copied != prefixLength) return false;

        std::vector<InputPart> original;
        original.reserve(partCount);
        for (size_t index = 0; index < partCount; index++) {
            original.emplace_back(std::move(part(index)));
        }
        extraParts.clear();
        partCount = 0;
        cursorPart = 0;
        cursorOffset = 0;
        addOwned(std::move(prefix));

        size_t consume = prefixLength;
        for (InputPart &current : original) {
            if (consume >= current.length) {
                consume -= current.length;
                current.release(env);
                continue;
            }
            if (consume) {
                current.pointer += consume;
                current.length -= consume;
                if (!current.owned.empty()) current.ownedOffset += consume;
                consume = 0;
            }
            appendPart() = std::move(current);
        }
        return prepare();
    }

    std::pair<iovec *, size_t> remainingIovecs() {
        const size_t count = std::min(partCount - cursorPart, maximumIOVectors());
        iovec *result = inlineIovecs.data();
        if (count > INLINE_PARTS) {
            iovecs.resize(count);
            result = iovecs.data();
        }
        for (size_t output = 0, index = cursorPart; output < count;
             output++, index++) {
            InputPart &current = part(index);
            const size_t offset = index == cursorPart ? cursorOffset : 0;
            result[output].iov_base = current.pointer
                ? current.pointer + offset
                : nullptr;
            result[output].iov_len = current.length - offset;
        }
        return {result, count};
    }

    const char *remainingData() {
        InputPart &current = part(cursorPart);
        return current.pointer ? current.pointer + cursorOffset : nullptr;
    }

    size_t remainingPartBytes() const {
        const InputPart &current = part(cursorPart);
        return current.length - cursorOffset;
    }

    size_t remainingBytes() const { return bytes - sent; }

    void retainRemainingInputs() {
        for (size_t index = cursorPart; index < partCount; index++) {
            InputPart &current = part(index);
            if (current.source) retainBackingStore(current.source, current);
        }
    }

    void advance(size_t length) {
        sent += length;
        advanceCursor(length);
    }

    void stageTLS(size_t length) {
        advanceCursor(length);
    }

    void commitTLS(size_t length) {
        sent += length;
    }

    size_t unstagedBytes(size_t staged) const {
        return bytes - sent - staged;
    }

    InputPart &front() { return inlineParts[0]; }

private:
    void advanceCursor(size_t length) {
        while (length && cursorPart < partCount) {
            InputPart &current = part(cursorPart);
            const size_t available = current.length - cursorOffset;
            if (length < available) {
                cursorOffset += length;
                return;
            }
            length -= available;
            cursorPart++;
            cursorOffset = 0;
        }
        skipEmptyParts();
    }

    InputPart &part(size_t index) {
        return index < INLINE_PARTS ? inlineParts[index] : extraParts[index - INLINE_PARTS];
    }

    const InputPart &part(size_t index) const {
        return index < INLINE_PARTS ? inlineParts[index] : extraParts[index - INLINE_PARTS];
    }

    InputPart &appendPart() {
        if (partCount < INLINE_PARTS) return inlineParts[partCount++];
        extraParts.emplace_back();
        partCount++;
        return extraParts.back();
    }

    void removeLastPart() {
        if (partCount > INLINE_PARTS) extraParts.pop_back();
        if (partCount) partCount--;
    }

    void skipEmptyParts() {
        while (cursorPart < partCount && part(cursorPart).length == 0) cursorPart++;
    }

public:
    napi_env env;
    std::array<InputPart, INLINE_PARTS> inlineParts;
    std::array<char, INLINE_BYTES> inlineBytes;
    std::vector<InputPart> extraParts;
    std::array<iovec, INLINE_PARTS> inlineIovecs;
    std::vector<iovec> iovecs;
    size_t partCount = 0;
    size_t bytes = 0;
    size_t sent = 0;
    size_t cursorPart = 0;
    size_t cursorOffset = 0;
    napi_ref callback = nullptr;
    napi_value callbackValue = nullptr;
    bool deferred = false;
    WriteRequest *next = nullptr;

};

WriteRequest::Cache::~Cache() {
    while (head) {
        FreeNode *node = head;
        head = node->next;
        ::operator delete(node);
    }
}

thread_local WriteRequest::Cache WriteRequest::cache_;

using NewSessionCallback = int (*)(SSL *, SSL_SESSION *);
using GetSessionCallback = SSL_SESSION *(*)(SSL *, const unsigned char *, int, int *);

struct TLSContextCallbacks {
    NewSessionCallback newSession = nullptr;
    GetSessionCallback getSession = nullptr;
    SSL_CTX_keylog_cb_func keylog = nullptr;
    size_t owners = 0;
};

std::mutex tlsCallbackMutex;
std::unordered_map<SSL_CTX *, TLSContextCallbacks> tlsContexts;
std::unordered_set<const SSL *> ownedTLSConnections;

int multiplexNewSession(SSL *ssl, SSL_SESSION *session) {
    NewSessionCallback callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(tlsCallbackMutex);
        if (ownedTLSConnections.contains(ssl)) return 0;
        auto iterator = tlsContexts.find(SSL_get_SSL_CTX(ssl));
        if (iterator != tlsContexts.end()) callback = iterator->second.newSession;
    }
    return callback && callback != multiplexNewSession ? callback(ssl, session) : 0;
}

SSL_SESSION *multiplexGetSession(
    SSL *ssl, const unsigned char *data, int length, int *copy) {
    GetSessionCallback callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(tlsCallbackMutex);
        if (ownedTLSConnections.contains(ssl)) {
            *copy = 0;
            return nullptr;
        }
        auto iterator = tlsContexts.find(SSL_get_SSL_CTX(ssl));
        if (iterator != tlsContexts.end()) callback = iterator->second.getSession;
    }
    return callback && callback != multiplexGetSession
        ? callback(ssl, data, length, copy)
        : nullptr;
}

void multiplexKeylog(const SSL *ssl, const char *line) {
    SSL_CTX_keylog_cb_func callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(tlsCallbackMutex);
        if (ownedTLSConnections.contains(ssl)) return;
        auto iterator = tlsContexts.find(SSL_get_SSL_CTX(ssl));
        if (iterator != tlsContexts.end()) callback = iterator->second.keylog;
    }
    if (callback && callback != multiplexKeylog) callback(ssl, line);
}

void retainTLSContext(SSL_CTX *context) {
    if (!context) return;
    std::lock_guard<std::mutex> lock(tlsCallbackMutex);
    TLSContextCallbacks &callbacks = tlsContexts[context];
    if (callbacks.owners++ != 0) return;
    callbacks.newSession = SSL_CTX_sess_get_new_cb(context);
    callbacks.getSession = SSL_CTX_sess_get_get_cb(context);
    callbacks.keylog = SSL_CTX_get_keylog_callback(context);
    SSL_CTX_sess_set_new_cb(context, multiplexNewSession);
    SSL_CTX_sess_set_get_cb(context, multiplexGetSession);
    SSL_CTX_set_keylog_callback(context, multiplexKeylog);
}

void releaseTLSContext(SSL_CTX *context) {
    if (!context) return;
    std::lock_guard<std::mutex> lock(tlsCallbackMutex);
    auto iterator = tlsContexts.find(context);
    if (iterator == tlsContexts.end() || --iterator->second.owners != 0) return;
    if (SSL_CTX_sess_get_new_cb(context) == multiplexNewSession) {
        SSL_CTX_sess_set_new_cb(context, iterator->second.newSession);
    }
    if (SSL_CTX_sess_get_get_cb(context) == multiplexGetSession) {
        SSL_CTX_sess_set_get_cb(context, iterator->second.getSession);
    }
    if (SSL_CTX_get_keylog_callback(context) == multiplexKeylog) {
        SSL_CTX_set_keylog_callback(context, iterator->second.keylog);
    }
    tlsContexts.erase(iterator);
}

void retainTLSCallbacks(SSL *ssl, SSL_CTX *initialContext) {
    SSL_CTX *currentContext = SSL_get_SSL_CTX(ssl);
    retainTLSContext(initialContext);
    if (currentContext != initialContext) retainTLSContext(currentContext);
    {
        std::lock_guard<std::mutex> lock(tlsCallbackMutex);
        ownedTLSConnections.insert(ssl);
    }
    SSL_set_info_callback(ssl, nullptr);
    SSL_set_cert_cb(ssl, nullptr, nullptr);
    SSL_set_msg_callback(ssl, nullptr);
    SSL_set_msg_callback_arg(ssl, nullptr);
#ifdef SSL_OP_NO_RENEGOTIATION
    SSL_set_options(ssl, SSL_OP_NO_RENEGOTIATION);
#endif
    SSL_set_app_data(ssl, nullptr);
}

void releaseTLSCallbacks(SSL *ssl, SSL_CTX *initialContext) {
    if (!ssl) return;
    SSL_CTX *currentContext = SSL_get_SSL_CTX(ssl);
    {
        std::lock_guard<std::mutex> lock(tlsCallbackMutex);
        ownedTLSConnections.erase(ssl);
    }
    if (currentContext != initialContext) releaseTLSContext(currentContext);
    releaseTLSContext(initialContext);
}

int duplicateDescriptor(int descriptor) {
    int duplicate = -1;
#ifdef F_DUPFD_CLOEXEC
    duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
    if (duplicate >= 0 || errno != EINVAL) return duplicate;
#endif
    duplicate = dup(descriptor);
    if (duplicate < 0) return duplicate;
    const int flags = fcntl(duplicate, F_GETFD, 0);
    if (flags < 0 || fcntl(duplicate, F_SETFD, flags | FD_CLOEXEC) < 0) {
        const int error = errno;
        close(duplicate);
        errno = error;
        return -1;
    }
    return duplicate;
}

bool wouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

std::string sslErrorMessage(const char *operation) {
    const unsigned long code = ERR_get_error();
    if (!code) return std::string(operation) + " failed";
    std::array<char, 256> buffer{};
    ERR_error_string_n(code, buffer.data(), buffer.size());
    return std::string(operation) + " failed: " + buffer.data();
}

} // namespace

struct NativeEnvironment {
    napi_env env = nullptr;
    napi_async_cleanup_hook_handle cleanupHook = nullptr;
    NativeTransport *head = nullptr;
    bool cleaning = false;
};

namespace {

std::mutex nativeEnvironmentMutex;
std::unordered_map<napi_env, NativeEnvironment *> nativeEnvironments;

void completeNativeEnvironmentCleanup(NativeEnvironment *environment);

NativeEnvironment *getNativeEnvironment(napi_env env) {
    std::lock_guard<std::mutex> lock(nativeEnvironmentMutex);
    const auto iterator = nativeEnvironments.find(env);
    return iterator == nativeEnvironments.end() ? nullptr : iterator->second;
}

} // namespace

class NativeTransport final {
public:
    enum class TLSState : unsigned char {
        NONE,
        MEMORY_BIO
    };

    NativeTransport(napi_env env,
                    uv_loop_t *loop,
                    int descriptor,
                    SSL *ssl,
                    SSL_CTX *initialTLSContext,
                    eioWS::StreamWebSocket *session,
                    napi_value owner,
                    bool textAsBuffer,
                    size_t maxBackpressure,
                    NativeEnvironment *environment,
                    NativeTransport **storage) :
        env_(env),
        loop_(loop),
        descriptor_(descriptor),
        ssl_(ssl),
        initialTLSContext_(initialTLSContext),
        session_(session),
        textAsBuffer_(textAsBuffer),
        maxBackpressure_(maxBackpressure),
        tlsState_(ssl ? TLSState::MEMORY_BIO : TLSState::NONE),
        environment_(environment),
        storage_(storage) {
        if (!checkStatus(env_, napi_create_reference(env_, owner, 1, &owner_),
                         "failed to retain native WebSocket owner")) {
            return;
        }
        napi_value messageCallback = nullptr;
        if (!checkStatus(env_,
                         napi_get_named_property(
                             env_, owner, "_onNativeMessage", &messageCallback),
                         "failed to access native message callback") ||
            !checkStatus(env_,
                         napi_create_reference(env_, messageCallback, 1, &messageCallback_),
                         "failed to retain native message callback")) {
            return;
        }
        v8::Local<v8::Value> resource = localValue(owner);
        if (!resource->IsObject()) {
            napi_throw_type_error(env_, nullptr, "native WebSocket owner must be an object");
            return;
        }
        asyncResource_ = new (std::nothrow) node::AsyncResource(
            v8::Isolate::GetCurrent(),
            resource.As<v8::Object>(),
            "eiows.ownedTransport");
        if (!asyncResource_) {
            napi_throw_error(env_, nullptr, "failed to initialize native transport resource");
            return;
        }
        valid_ = true;
        linkEnvironment();
    }

    ~NativeTransport() {
        cancelWrites(UV_ECANCELED);
        if (ssl_) {
            if (tlsCallbacksRetained_) {
                releaseTLSCallbacks(ssl_, initialTLSContext_);
            }
            SSL_free(ssl_);
        }
        if (initialTLSContext_) SSL_CTX_free(initialTLSContext_);
        if (descriptor_ >= 0) close(descriptor_);
        delete asyncResource_;
        if (!environmentCleaning_) {
            if (messageCallback_) napi_delete_reference(env_, messageCallback_);
            if (owner_) napi_delete_reference(env_, owner_);
        }
        unlinkEnvironment();
    }

    bool valid() const { return valid_; }

    int activate() {
        if (!valid_ || active_ || closing_) return UV_EINVAL;
        if (ssl_) {
            retainTLSCallbacks(ssl_, initialTLSContext_);
            tlsCallbacksRetained_ = true;
            nodeReadBIO_ = SSL_get_rbio(ssl_);
            nodeWriteBIO_ = SSL_get_wbio(ssl_);
            if (!nodeReadBIO_ || !nodeWriteBIO_) return UV_EINVAL;
            SSL_set_mode(ssl_, SSL_MODE_RELEASE_BUFFERS);
        }
        const int status = uv_poll_init_socket(loop_, &poll_, descriptor_);
        if (status < 0) return status;
        poll_.data = this;
        pollInitialized_ = true;
        active_ = true;

        if (ssl_ && !processMemoryBIO()) return UV_EIO;
        flushWrites();
        updatePoll();
        return closing_ ? UV_EIO : 0;
    }

    void requestDestroy() {
        destroyRequested_ = true;
        session_ = nullptr;
        if (!active_) {
            delete this;
            return;
        }
        if (!closing_) closeNow();
        if (pollClosed_ && !insideCloseCallback_) delete this;
    }

    void terminate() {
        if (!closing_) closeNow();
    }

    void detachStorage() { storage_ = nullptr; }

    NativeTransport *nextEnvironmentTransport() const { return environmentNext_; }

    void beginEnvironmentCleanup() {
        environmentCleaning_ = true;
        destroyRequested_ = true;
        session_ = nullptr;
        if (storage_) {
            if (*storage_ == this) *storage_ = nullptr;
            storage_ = nullptr;
        }
        if (!closing_) closeNow();
        if (!pollInitialized_ || pollClosed_) finishEnvironmentCleanup();
    }

    bool feed(napi_value source, char *data, size_t length) {
        if (!session_ || closing_) return false;
        return consume(data, length, source, data, length);
    }

    int writeMessage(napi_value input,
                     eioWS::OpCode opCode,
                     bool compress,
                     napi_value callback) {
        if (!writable()) return UV_EBADF;
        auto request = std::make_unique<WriteRequest>(env_);
        if (!request->retainCallback(callback)) return UV_EINVAL;
        // Engine.IO normalizes text packets to strings before reaching this binding.
        if (!(opCode == eioWS::TEXT ? request->addTextInput(input)
                                    : request->addInput(input))) {
            return UV_EINVAL;
        }

        if (compress) {
            InputPart &source = request->front();
            std::string frame;
            if (!session_->createFrame(source.pointer, source.length, opCode, true, frame)) {
                napi_throw_error(env_, nullptr,
                                 "cannot send WebSocket frame in the current state");
                return UV_EINVAL;
            }
            request->clearParts();
            request->addOwned(std::move(frame));
        } else {
            request->prependOwned(frameHeader(request->front().length, opCode));
        }
        return submit(std::move(request));
    }

    int writeFrameList(napi_value list, napi_value callback) {
        if (!writable()) return UV_EBADF;
        bool isArray = false;
        if (!checkStatus(env_, napi_is_array(env_, list, &isArray),
                         "failed to inspect frame list")) {
            return UV_EINVAL;
        }
        if (!isArray) {
            napi_throw_type_error(env_, nullptr, "invalid pre-encoded frame");
            return UV_EINVAL;
        }
        uint32_t length = 0;
        if (!checkStatus(env_, napi_get_array_length(env_, list, &length),
                         "failed to read frame list") || length == 0) {
            if (length == 0) napi_throw_type_error(env_, nullptr, "invalid pre-encoded frame");
            return UV_EINVAL;
        }

        auto request = std::make_unique<WriteRequest>(env_);
        if (length > WriteRequest::INLINE_PARTS) {
            request->extraParts.reserve(length - WriteRequest::INLINE_PARTS);
        }
        if (!request->retainCallback(callback)) return UV_EINVAL;
        for (uint32_t index = 0; index < length; index++) {
            napi_value value = nullptr;
            if (!checkStatus(env_, napi_get_element(env_, list, index, &value),
                             "failed to read frame part") ||
                !request->addInput(value)) {
                return UV_EINVAL;
            }
        }
        return submit(std::move(request));
    }

    int writeClose(uint16_t code, const char *reason, size_t reasonLength) {
        if (!writable()) return UV_EBADF;
        std::string frame;
        if (!session_->createCloseFrame(code, reason, reasonLength, frame)) {
            return WRITE_NOT_SENT;
        }
        return writeOwned(std::move(frame));
    }

    size_t bufferedAmount() const { return pendingBytes_; }

private:
    void linkEnvironment() {
        if (!environment_) return;
        environmentNext_ = environment_->head;
        if (environmentNext_) environmentNext_->environmentPrevious_ = this;
        environment_->head = this;
        environmentRegistered_ = true;
    }

    void unlinkEnvironment() {
        if (!environmentRegistered_ || !environment_) return;
        if (environmentPrevious_) environmentPrevious_->environmentNext_ = environmentNext_;
        else environment_->head = environmentNext_;
        if (environmentNext_) environmentNext_->environmentPrevious_ = environmentPrevious_;
        environmentPrevious_ = nullptr;
        environmentNext_ = nullptr;
        environmentRegistered_ = false;
    }

    void finishEnvironmentCleanup() {
        NativeEnvironment *environment = environment_;
        unlinkEnvironment();
        environment_ = nullptr;
        delete this;
        completeNativeEnvironmentCleanup(environment);
    }

    static void onPoll(uv_poll_t *poll, int status, int events) {
        NativeTransport *transport = static_cast<NativeTransport *>(poll->data);
        napi_handle_scope scope = nullptr;
        if (napi_open_handle_scope(transport->env_, &scope) != napi_ok) {
            transport->closeNow();
            return;
        }
        try {
            transport->poll(status, events);
        } catch (const std::bad_alloc &) {
            transport->reportTransportError("native WebSocket allocation failed");
            transport->closeNow();
        } catch (const std::exception &error) {
            transport->reportTransportError(error.what());
            transport->closeNow();
        } catch (...) {
            transport->reportTransportError("unknown native WebSocket transport failure");
            transport->closeNow();
        }
        napi_close_handle_scope(transport->env_, scope);
    }

    static void onPollClosed(uv_handle_t *handle) {
        NativeTransport *transport = static_cast<NativeTransport *>(handle->data);
        transport->pollClosed_ = true;
        if (transport->environmentCleaning_) {
            transport->finishEnvironmentCleanup();
            return;
        }
        napi_handle_scope scope = nullptr;
        const bool scopeOpen = napi_open_handle_scope(transport->env_, &scope) == napi_ok;
        transport->insideCloseCallback_ = true;
        if (scopeOpen) transport->callOwner("_onNativeClosed", 0, nullptr);
        transport->insideCloseCallback_ = false;
        const bool destroy = transport->destroyRequested_;
        if (scopeOpen) napi_close_handle_scope(transport->env_, scope);
        if (destroy) delete transport;
    }

    bool writable() const {
        return valid_ && !closing_ && session_ && descriptor_ >= 0;
    }

    void poll(int status, int events) {
        if (closing_) return;
        if (status < 0) {
            fail(uv_strerror(status));
            return;
        }
        if (events & UV_READABLE) {
            const bool retryWrite = tlsWriteBlockedOnRead_;
            tlsWriteBlockedOnRead_ = false;
            readAvailable();
            if (!closing_ && retryWrite) flushWrites();
        }
        if (!closing_ && (events & UV_WRITABLE)) {
            if (tlsShutdownPending_) {
                performTLSShutdown();
                if (closing_) return;
            }
            if (tlsState_ == TLSState::MEMORY_BIO) processMemoryBIO();
            if (!closing_) flushWrites();
            if (!closing_ && tlsShutdownPending_) performTLSShutdown();
        }
        if (!closing_) updatePoll();
    }

    void readAvailable() {
        if (tlsState_ == TLSState::NONE) {
            readTCP();
        } else {
            readTLSMemoryBIO();
        }
    }

    void readTCP() {
        for (;;) {
            const ssize_t length = recv(
                descriptor_, sharedReadBuffer.data(), sharedReadBuffer.size(), 0);
            if (length > 0) {
                consume(sharedReadBuffer.data(), static_cast<size_t>(length));
                return;
            }
            if (length == 0) {
                closeNow();
                return;
            }
            if (errno == EINTR) continue;
            if (wouldBlock()) return;
            fail(std::strerror(errno));
            return;
        }
    }

    void readTLSMemoryBIO() {
        while (!closing_) {
            const ssize_t length = recv(
                descriptor_, sharedReadBuffer.data(), sharedReadBuffer.size(), 0);
            if (length > 0) {
                size_t written = 0;
                while (written < static_cast<size_t>(length)) {
                    const int result = BIO_write(
                        nodeReadBIO_,
                        sharedReadBuffer.data() + written,
                        static_cast<int>(std::min<size_t>(
                            static_cast<size_t>(length) - written,
                            std::numeric_limits<int>::max())));
                    if (result <= 0) {
                        fail("failed to transfer encrypted TLS input");
                        return;
                    }
                    written += static_cast<size_t>(result);
                }
                processMemoryBIO();
                return;
            }
            if (length == 0) {
                closeNow();
                return;
            }
            if (errno == EINTR) continue;
            if (wouldBlock()) {
                processMemoryBIO();
                return;
            }
            fail(std::strerror(errno));
            return;
        }
    }

    bool processMemoryBIO() {
        if (tlsState_ != TLSState::MEMORY_BIO || closing_) return !closing_;

        if (tlsProcessingInput_) return flushMemoryBIOOutput();
        struct ProcessingGuard {
            explicit ProcessingGuard(bool &value) : value(value) { value = true; }
            ~ProcessingGuard() { value = false; }
            bool &value;
        } guard(tlsProcessingInput_);

        while (!closing_) {
            size_t length = 0;
            ERR_clear_error();
            const int result = SSL_read_ex(
                ssl_, sharedReadBuffer.data(), sharedReadBuffer.size(), &length);
            if (result == 1 && length) {
                if (!consume(sharedReadBuffer.data(), length)) return false;
                continue;
            }
            const int error = SSL_get_error(ssl_, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) break;
            if (error == SSL_ERROR_ZERO_RETURN) {
                closeNow();
                return false;
            }
            fail(sslErrorMessage("SSL_read during TLS transfer"));
            return false;
        }

        return flushMemoryBIOOutput();
    }

    bool flushMemoryBIOOutput() {
        if (tlsCiphertextOffset_ < tlsCiphertext_.size()) {
            const char *data = tlsCiphertext_.data() + tlsCiphertextOffset_;
            const size_t remaining = tlsCiphertext_.size() - tlsCiphertextOffset_;
            ssize_t written;
            do {
                written = sendRaw(data, remaining);
            } while (written < 0 && errno == EINTR);
            if (written > 0) {
                tlsCiphertextOffset_ += static_cast<size_t>(written);
                if (tlsCiphertextOffset_ == tlsCiphertext_.size()) {
                    std::string().swap(tlsCiphertext_);
                    tlsCiphertextOffset_ = 0;
                }
            } else if (written < 0 && !wouldBlock()) {
                fail(std::strerror(errno));
                return false;
            }
            if (tlsCiphertextOffset_ < tlsCiphertext_.size()) return true;
        }

        while (const size_t pending = BIO_ctrl_pending(nodeWriteBIO_)) {
            const size_t batch = std::min(pending, TLS_OUTPUT_BATCH_SIZE);
            sharedTLSOutputBuffer.resize(batch);
            size_t offset = 0;
            while (offset < batch) {
                const int length = BIO_read(
                    nodeWriteBIO_,
                    sharedTLSOutputBuffer.data() + offset,
                    static_cast<int>(std::min<size_t>(
                        batch - offset, std::numeric_limits<int>::max())));
                if (length <= 0) {
                    fail("failed to drain encrypted TLS output");
                    return false;
                }
                offset += static_cast<size_t>(length);
            }

            ssize_t written;
            do {
                written = sendRaw(sharedTLSOutputBuffer.data(), batch);
            } while (written < 0 && errno == EINTR);
            if (written == static_cast<ssize_t>(batch)) continue;
            if (written < 0 && !wouldBlock()) {
                fail(std::strerror(errno));
                return false;
            }
            const size_t sent = written > 0 ? static_cast<size_t>(written) : 0;
            tlsCiphertext_.assign(
                sharedTLSOutputBuffer.data() + sent,
                sharedTLSOutputBuffer.data() + batch);
            tlsCiphertextOffset_ = 0;
            return true;
        }
        return true;
    }

    bool hasTLSOutput() const {
        return tlsCiphertextOffset_ < tlsCiphertext_.size() ||
            (nodeWriteBIO_ && BIO_ctrl_pending(nodeWriteBIO_) != 0);
    }

    ssize_t sendRaw(const void *data, size_t length) {
#ifdef MSG_NOSIGNAL
        return send(descriptor_, data, length, MSG_NOSIGNAL);
#else
        return send(descriptor_, data, length, 0);
#endif
    }

    bool consume(char *data,
                 size_t length,
                 napi_value source = nullptr,
                 const char *sourceData = nullptr,
                 size_t sourceLength = 0) {
        std::vector<eioWS::StreamWebSocketEvent> *events = nullptr;
        try {
            events = &session_->consume(data, length);
        } catch (const std::exception &error) {
            fail(error.what());
            return false;
        } catch (...) {
            fail("unknown native WebSocket parser failure");
            return false;
        }
        struct ClearEvents {
            std::vector<eioWS::StreamWebSocketEvent> *events;
            ~ClearEvents() { events->clear(); }
        } clear{events};

        for (eioWS::StreamWebSocketEvent &event : *events) {
            if (!session_ || closing_) return false;
            if (event.type == eioWS::StreamWebSocketEvent::Type::FRAME) {
                const int status = writeOwned(std::move(event.data));
                if (status < 0) {
                    fail(uv_strerror(status));
                    return false;
                }
            } else if (event.type == eioWS::StreamWebSocketEvent::Type::MESSAGE) {
                if (!dispatchMessage(event, source, sourceData, sourceLength)) return false;
            } else {
                pendingClose_ = true;
                pendingCloseCode_ = event.code;
                pendingCloseReason_.assign(event.payloadData(), event.payloadLength());
                maybeFinishPeerClose();
            }
        }
        return !closing_;
    }

    bool dispatchMessage(eioWS::StreamWebSocketEvent &event,
                         napi_value source,
                         const char *sourceData,
                         size_t sourceLength) {
        napi_value message = nullptr;
        if (event.opCode == eioWS::BINARY || textAsBuffer_) {
            message = event.view
                ? createBufferSlice(source,
                                    sourceData,
                                    sourceLength,
                                    event.payloadData(),
                                    event.payloadLength())
                : createOwnedBuffer(env_, std::move(event.data));
        } else {
            message = createString(env_, event.payloadData(), event.payloadLength());
        }
        napi_value binary = nullptr;
        if (!message ||
            !checkStatus(env_, napi_get_boolean(env_, event.opCode == eioWS::BINARY, &binary),
                         "failed to create binary flag")) {
            return false;
        }
        napi_value arguments[] = {message, binary};
        return callOwnerReference(messageCallback_, 2, arguments);
    }

    void maybeFinishPeerClose() {
        if (!pendingClose_ || writeHead_ || closing_) return;
        pendingClose_ = false;
        napi_value code = nullptr;
        napi_value reason = createString(
            env_, pendingCloseReason_.data(), pendingCloseReason_.size());
        if (!reason ||
            !checkStatus(env_, napi_create_uint32(env_, pendingCloseCode_, &code),
                         "failed to create close code")) {
            fail("failed to dispatch WebSocket close");
            return;
        }
        napi_value arguments[] = {code, reason};
        if (!callOwner("_onNativeClose", 2, arguments)) return;
        gracefulCloseRequested_ = true;
        performTLSShutdown();
    }

    void performTLSShutdown() {
        if (!gracefulCloseRequested_ || closing_ || writeHead_) return;
        if (!ssl_) {
            closeNow();
            return;
        }
        if (tlsState_ == TLSState::MEMORY_BIO) {
            if (tlsShutdownGenerated_) {
                if (!flushMemoryBIOOutput()) return;
                if (hasTLSOutput()) {
                    tlsShutdownPending_ = true;
                    return;
                }
                tlsShutdownPending_ = false;
                closeNow();
                return;
            }
            if (!processMemoryBIO() || hasTLSOutput()) {
                tlsShutdownPending_ = true;
                return;
            }
            ERR_clear_error();
            const int result = SSL_shutdown(ssl_);
            const int error = result < 0 ? SSL_get_error(ssl_, result) : SSL_ERROR_NONE;
            if (result >= 0 || error == SSL_ERROR_WANT_READ ||
                error == SSL_ERROR_WANT_WRITE) {
                tlsShutdownGenerated_ = true;
                if (!flushMemoryBIOOutput()) return;
                if (hasTLSOutput()) {
                    tlsShutdownPending_ = true;
                    return;
                }
            }
            tlsShutdownPending_ = false;
            closeNow();
            return;
        }
    }

    int writeOwned(std::string value) {
        auto request = std::make_unique<WriteRequest>(env_);
        request->addOwned(std::move(value));
        return submit(std::move(request));
    }

    int submit(std::unique_ptr<WriteRequest> request) {
        if (!writable()) return UV_EBADF;
        if (!request->prepare()) return UV_EINVAL;
        if (request->bytes == 0) return WRITE_SYNCHRONOUS;
        if (pendingBytes_ > std::numeric_limits<size_t>::max() - request->bytes) {
            napi_throw_range_error(env_, nullptr, "native write queue is too large");
            return UV_EOVERFLOW;
        }
        if (ssl_ && request->partCount > 1) {
            if (request->bytes <= TLS_FULL_COALESCE_LIMIT) {
                if (!request->flatten()) return UV_EINVAL;
            } else {
                if (!request->coalescePrefix(TLS_COALESCE_PREFIX)) return UV_EINVAL;
            }
        }

        WriteRequest *submitted = request.get();
        const bool alreadyQueued = writeHead_ != nullptr || flushingWrites_;
        if (alreadyQueued && !request->defer()) return UV_EINVAL;
        pendingBytes_ += request->bytes;
        enqueueWrite(request.release());
        if (alreadyQueued) {
            if (backpressureExceeded()) {
                terminateForBackpressure();
                return WRITE_ASYNCHRONOUS;
            }
            updatePoll();
            return WRITE_ASYNCHRONOUS;
        }

        flushWrites();
        if (closing_) return UV_EIO;
        for (WriteRequest *current = writeHead_; current; current = current->next) {
            if (current == submitted) {
                if (!current->defer()) {
                    cancelWrites(UV_ECANCELED);
                    return UV_EINVAL;
                }
                if (backpressureExceeded()) terminateForBackpressure();
                if (closing_) return WRITE_ASYNCHRONOUS;
                updatePoll();
                return WRITE_ASYNCHRONOUS;
            }
        }
        return WRITE_SYNCHRONOUS;
    }

    void flushWrites() {
        if (flushingWrites_ || closing_ || !active_) return;
        // Activation and readTLSMemoryBIO() drive encrypted input. An outbound
        // submission only needs to drain ciphertext already produced by SSL.
        if (tlsState_ == TLSState::MEMORY_BIO && !flushMemoryBIOOutput()) return;
        if (tlsState_ == TLSState::MEMORY_BIO && hasTLSOutput()) return;

        flushingWrites_ = true;
        while (writeHead_ && !closing_) {
            WriteRequest *request = writeHead_;
            ssize_t written = ssl_ ? writeTLS(*request) : writeTCP(*request);
            if (written > 0) {
                if (ssl_) request->commitTLS(static_cast<size_t>(written));
                else request->advance(static_cast<size_t>(written));
                pendingBytes_ -= std::min(pendingBytes_, static_cast<size_t>(written));
                if (request->remainingBytes() == 0) completeHeadWrite(0);
                continue;
            }
            if (written == 0) break;
            const int error = errno ? -errno : UV_EIO;
            flushingWrites_ = false;
            fail(uv_strerror(error));
            return;
        }
        flushingWrites_ = false;
        if (!writeHead_) maybeFinishPeerClose();
    }

    ssize_t writeTCP(WriteRequest &request) {
        auto [iovecs, count] = request.remainingIovecs();
        msghdr message{};
        message.msg_iov = iovecs;
        message.msg_iovlen = count;
        ssize_t result;
        do {
#ifdef MSG_NOSIGNAL
            result = sendmsg(descriptor_, &message, MSG_NOSIGNAL);
#else
            result = sendmsg(descriptor_, &message, 0);
#endif
        } while (result < 0 && errno == EINTR);
        if (result < 0 && wouldBlock()) return 0;
        return result;
    }

    ssize_t writeTLS(WriteRequest &request) {
        if (tlsCommittedPlaintext_) {
            if (!flushMemoryBIOOutput()) return -1;
            if (hasTLSOutput()) return 0;
            const size_t committed = tlsCommittedPlaintext_;
            tlsCommittedPlaintext_ = 0;
            return static_cast<ssize_t>(committed);
        }

        size_t batchRemaining = TLS_PLAINTEXT_BATCH_SIZE;
        while (request.unstagedBytes(tlsCommittedPlaintext_) && batchRemaining) {
            const size_t batch = std::min(
                request.remainingPartBytes(), batchRemaining);
            size_t written = 0;
            errno = 0;
            ERR_clear_error();
            const int result = SSL_write_ex(
                ssl_, request.remainingData(), batch, &written);
            if (result == 1 && written) {
                tlsWriteBlockedOnRead_ = false;
                request.stageTLS(written);
                tlsCommittedPlaintext_ += written;
                batchRemaining -= written;
                continue;
            }
            const int error = SSL_get_error(ssl_, result);
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                tlsWriteBlockedOnRead_ = error == SSL_ERROR_WANT_READ;
                break;
            }
            errno = EIO;
            return -1;
        }

        if (!tlsCommittedPlaintext_) {
            if (!flushMemoryBIOOutput()) return -1;
            return 0;
        }
        if (!flushMemoryBIOOutput()) return -1;
        if (hasTLSOutput()) return 0;
        const size_t committed = tlsCommittedPlaintext_;
        tlsCommittedPlaintext_ = 0;
        return static_cast<ssize_t>(committed);
    }

    void completeHeadWrite(int status) {
        std::unique_ptr<WriteRequest> request(dequeueWrite());
        if (request->deferred) invokeWriteCallback(*request, status);
    }

    void cancelWrites(int status) {
        while (writeHead_) {
            std::unique_ptr<WriteRequest> request(dequeueWrite());
            pendingBytes_ -= std::min(pendingBytes_, request->remainingBytes());
            if (environmentCleaning_) request->abandonReferences();
            else if (request->deferred) invokeWriteCallback(*request, status);
        }
    }

    void enqueueWrite(WriteRequest *request) {
        request->next = nullptr;
        if (writeTail_) writeTail_->next = request;
        else writeHead_ = request;
        writeTail_ = request;
    }

    WriteRequest *dequeueWrite() {
        WriteRequest *request = writeHead_;
        writeHead_ = request->next;
        if (!writeHead_) writeTail_ = nullptr;
        request->next = nullptr;
        return request;
    }

    void updatePoll() {
        if (!active_ || closing_) return;
        int events = UV_READABLE;
        if ((writeHead_ && (!ssl_ || !tlsWriteBlockedOnRead_)) ||
            tlsShutdownPending_ ||
            (tlsState_ == TLSState::MEMORY_BIO &&
             hasTLSOutput())) {
            events |= UV_WRITABLE;
        }
        if (events == pollEvents_) return;
        const int status = uv_poll_start(&poll_, events, onPoll);
        if (status < 0) {
            fail(uv_strerror(status));
        } else {
            pollEvents_ = events;
        }
    }

    void fail(const std::string &message) {
        if (closing_) return;
        reportTransportError(message.c_str());
        closeNow();
    }

    bool backpressureExceeded() const {
        return maxBackpressure_ && pendingBytes_ > maxBackpressure_;
    }

    void terminateForBackpressure() {
        if (closing_) return;
        callOwner("_onNativeBackpressure", 0, nullptr);
        closeNow(UV_ENOBUFS);
    }

    void closeNow(int writeStatus = UV_ECANCELED) {
        if (closing_) return;
        closing_ = true;
        cancelWrites(writeStatus);
        std::string().swap(tlsCiphertext_);
        tlsCiphertextOffset_ = 0;
        tlsCommittedPlaintext_ = 0;
        if (pollInitialized_) {
            uv_poll_stop(&poll_);
            pollEvents_ = 0;
        }
        if (descriptor_ >= 0) {
            close(descriptor_);
            descriptor_ = -1;
        }
        if (pollInitialized_ && !uv_is_closing(reinterpret_cast<uv_handle_t *>(&poll_))) {
            uv_close(reinterpret_cast<uv_handle_t *>(&poll_), onPollClosed);
        } else {
            pollClosed_ = true;
        }
    }

    bool callOwner(const char *methodName, size_t argumentCount, napi_value *arguments) {
        napi_value owner = nullptr;
        napi_value callback = nullptr;
        if (!checkStatus(env_, napi_get_reference_value(env_, owner_, &owner),
                         "failed to access native WebSocket owner") ||
            !checkStatus(env_, napi_get_named_property(env_, owner, methodName, &callback),
                         "failed to access native WebSocket callback")) {
            return false;
        }
        return makeCallback(owner, callback, argumentCount, arguments);
    }

    bool callOwnerReference(
        napi_ref reference, size_t argumentCount, napi_value *arguments) {
        napi_value owner = nullptr;
        napi_value callback = nullptr;
        if (!checkStatus(env_, napi_get_reference_value(env_, owner_, &owner),
                         "failed to access native WebSocket owner") ||
            !checkStatus(env_, napi_get_reference_value(env_, reference, &callback),
                         "failed to access native WebSocket callback")) {
            return false;
        }
        return makeCallback(owner, callback, argumentCount, arguments);
    }

    bool makeCallback(napi_value,
                      napi_value callback,
                      size_t argumentCount,
                      napi_value *arguments) {
        if (!asyncResource_) return false;
        v8::Local<v8::Value> callbackValue = localValue(callback);
        if (!callbackValue->IsFunction()) {
            napi_throw_type_error(env_, nullptr, "native WebSocket callback is not a function");
            return false;
        }
        v8::Isolate *isolate = v8::Isolate::GetCurrent();
        v8::TryCatch tryCatch(isolate);
        const auto result = asyncResource_->MakeCallback(
            callbackValue.As<v8::Function>(),
            static_cast<int>(argumentCount),
            reinterpret_cast<v8::Local<v8::Value> *>(arguments));
        if (tryCatch.HasCaught()) {
            node::FatalException(isolate, tryCatch);
            return false;
        }
        return !result.IsEmpty();
    }

    void reportTransportError(const char *message) {
        if (reportingError_ || !owner_) return;
        reportingError_ = true;
        napi_value value = createString(env_, message, std::strlen(message));
        if (value) {
            napi_value arguments[] = {value};
            callOwner("_onNativeTransportError", 1, arguments);
        }
        reportingError_ = false;
    }

    void invokeWriteCallback(WriteRequest &request, int status) {
        if (!request.callback) return;
        napi_value callback = nullptr;
        napi_value owner = nullptr;
        if (!checkStatus(env_, napi_get_reference_value(env_, request.callback, &callback),
                         "failed to access native write callback") ||
            !checkStatus(env_, napi_get_reference_value(env_, owner_, &owner),
                         "failed to access native WebSocket owner")) {
            return;
        }
        napi_value argument = nullptr;
        if (status < 0) {
            napi_value message = createString(
                env_, uv_strerror(status), std::strlen(uv_strerror(status)));
            if (!message ||
                !checkStatus(env_, napi_create_error(env_, nullptr, message, &argument),
                             "failed to create write error")) {
                return;
            }
            if (status == UV_ENOBUFS) {
                napi_value code = createString(
                    env_, "EIOWS_MAX_BACKPRESSURE", sizeof("EIOWS_MAX_BACKPRESSURE") - 1);
                if (!code ||
                    !checkStatus(env_,
                                 napi_set_named_property(env_, argument, "code", code),
                                 "failed to set write error code")) {
                    return;
                }
            }
        } else if (!checkStatus(env_, napi_get_undefined(env_, &argument),
                                "failed to create undefined callback argument")) {
            return;
        }
        makeCallback(owner, callback, 1, &argument);
    }

    napi_env env_;
    uv_loop_t *loop_;
    int descriptor_;
    SSL *ssl_;
    SSL_CTX *initialTLSContext_;
    BIO *nodeReadBIO_ = nullptr;
    BIO *nodeWriteBIO_ = nullptr;
    eioWS::StreamWebSocket *session_;
    bool textAsBuffer_;
    size_t maxBackpressure_;
    TLSState tlsState_;
    uv_poll_t poll_{};
    napi_ref owner_ = nullptr;
    napi_ref messageCallback_ = nullptr;
    node::AsyncResource *asyncResource_ = nullptr;
    WriteRequest *writeHead_ = nullptr;
    WriteRequest *writeTail_ = nullptr;
    size_t pendingBytes_ = 0;
    int pollEvents_ = 0;
    std::string tlsCiphertext_;
    size_t tlsCiphertextOffset_ = 0;
    size_t tlsCommittedPlaintext_ = 0;
    std::string pendingCloseReason_;
    uint16_t pendingCloseCode_ = 1006;
    bool valid_ = false;
    bool active_ = false;
    bool pollInitialized_ = false;
    bool pollClosed_ = false;
    bool insideCloseCallback_ = false;
    bool closing_ = false;
    bool destroyRequested_ = false;
    bool flushingWrites_ = false;
    bool reportingError_ = false;
    bool pendingClose_ = false;
    bool gracefulCloseRequested_ = false;
    bool tlsWriteBlockedOnRead_ = false;
    bool tlsShutdownPending_ = false;
    bool tlsShutdownGenerated_ = false;
    bool tlsProcessingInput_ = false;
    bool tlsCallbacksRetained_ = false;
    bool environmentCleaning_ = false;
    bool environmentRegistered_ = false;
    NativeEnvironment *environment_ = nullptr;
    NativeTransport **storage_ = nullptr;
    NativeTransport *environmentPrevious_ = nullptr;
    NativeTransport *environmentNext_ = nullptr;
};

namespace {

void completeNativeEnvironmentCleanup(NativeEnvironment *environment) {
    if (!environment || environment->head) return;
    napi_async_cleanup_hook_handle hook = environment->cleanupHook;
    environment->cleanupHook = nullptr;
    if (hook) napi_remove_async_cleanup_hook(hook);
    delete environment;
}

void cleanupNativeEnvironment(napi_async_cleanup_hook_handle hook, void *data) {
    auto *environment = static_cast<NativeEnvironment *>(data);
    environment->cleaning = true;
    environment->cleanupHook = hook;
    {
        std::lock_guard<std::mutex> lock(nativeEnvironmentMutex);
        nativeEnvironments.erase(environment->env);
    }

    NativeTransport *transport = environment->head;
    if (!transport) {
        completeNativeEnvironmentCleanup(environment);
        return;
    }
    while (transport) {
        NativeTransport *next = transport->nextEnvironmentTransport();
        transport->beginEnvironmentCleanup();
        transport = next;
    }
}

} // namespace

bool initializeNativeEnvironment(napi_env env) {
    {
        std::lock_guard<std::mutex> lock(nativeEnvironmentMutex);
        if (nativeEnvironments.contains(env)) return true;
    }
    auto environment = std::make_unique<NativeEnvironment>();
    environment->env = env;
    if (napi_add_async_cleanup_hook(
            env,
            cleanupNativeEnvironment,
            environment.get(),
            &environment->cleanupHook) != napi_ok) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(nativeEnvironmentMutex);
        nativeEnvironments.emplace(env, environment.get());
    }
    environment.release();
    return true;
}

NativeTransport *attachNativeTransport(napi_env env,
                                       eioWS::StreamWebSocket *session,
                                       napi_value handle,
                                       napi_value owner,
                                       bool textAsBuffer,
                                       bool encrypted,
                                       size_t maxBackpressure,
                                       NativeTransport **storage) {
    NativeEnvironment *environment = getNativeEnvironment(env);
    if (!environment || environment->cleaning) return nullptr;
    v8::Local<v8::Value> handleValue = localValue(handle);
    if (handleValue.IsEmpty() || !handleValue->IsObject()) return nullptr;
    v8::Local<v8::Object> handleObject = handleValue.As<v8::Object>();
    if (handleObject->InternalFieldCount() <= node::StreamBase::kStreamBaseField) return nullptr;

    node::StreamBase *stream = node::StreamBase::FromObject(handleObject);
    if (!stream || !stream->IsAlive() || stream->IsClosing()) return nullptr;
    node::AsyncWrap *asyncWrap = stream->GetAsyncWrap();
    if (!asyncWrap) return nullptr;
    const node::AsyncWrap::ProviderType provider = asyncWrap->provider_type();
    if (encrypted != (provider == node::AsyncWrap::PROVIDER_TLSWRAP)) return nullptr;
    if (!encrypted && provider != node::AsyncWrap::PROVIDER_TCPWRAP) return nullptr;
    const int descriptor = stream->GetFD();
    if (descriptor < 0) return nullptr;

    SSL *ssl = nullptr;
    SSL_CTX *initialContext = nullptr;
    if (encrypted) {
        auto *tlsWrap = static_cast<node::crypto::TLSWrap *>(stream);
        ssl = getTLSWrapSSL(tlsWrap);
        initialContext = getTLSWrapInitialContext(tlsWrap);
        if (!ssl || !initialContext || SSL_up_ref(ssl) != 1) return nullptr;
        if (SSL_CTX_up_ref(initialContext) != 1) {
            SSL_free(ssl);
            return nullptr;
        }
    }

    const int ownedDescriptor = duplicateDescriptor(descriptor);
    if (ownedDescriptor < 0) {
        if (ssl) SSL_free(ssl);
        if (initialContext) SSL_CTX_free(initialContext);
        return nullptr;
    }
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (setsockopt(
            ownedDescriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
        close(ownedDescriptor);
        if (ssl) SSL_free(ssl);
        if (initialContext) SSL_CTX_free(initialContext);
        return nullptr;
    }
#endif

    uv_loop_t *loop = nullptr;
    if (napi_get_uv_event_loop(env, &loop) != napi_ok || !loop) {
        close(ownedDescriptor);
        if (ssl) SSL_free(ssl);
        if (initialContext) SSL_CTX_free(initialContext);
        return nullptr;
    }

    if (stream->ReadStop() < 0) {
        close(ownedDescriptor);
        if (ssl) SSL_free(ssl);
        if (initialContext) SSL_CTX_free(initialContext);
        return nullptr;
    }
    auto *transport = new NativeTransport(
        env,
        loop,
        ownedDescriptor,
        ssl,
        initialContext,
        session,
        owner,
        textAsBuffer,
        maxBackpressure,
        environment,
        storage);
    if (!transport->valid()) {
        delete transport;
        return nullptr;
    }
    return transport;
}

int activateNativeTransport(NativeTransport *transport) {
    return transport ? transport->activate() : UV_EBADF;
}

void terminateNativeTransport(NativeTransport *transport) {
    if (transport) transport->terminate();
}

void detachNativeTransportStorage(NativeTransport *transport) {
    if (transport) transport->detachStorage();
}

void destroyNativeTransport(NativeTransport *transport) {
    if (transport) transport->requestDestroy();
}

bool feedNativeTransport(NativeTransport *transport,
                         napi_value source,
                         char *data,
                         size_t length) {
    return transport && transport->feed(source, data, length);
}

int writeNativeMessage(NativeTransport *transport,
                       napi_value input,
                       eioWS::OpCode opCode,
                       bool compress,
                       napi_value callback) {
    return transport
        ? transport->writeMessage(input, opCode, compress, callback)
        : UV_EBADF;
}

int writeNativeFrameList(NativeTransport *transport,
                         napi_value list,
                         napi_value callback) {
    return transport ? transport->writeFrameList(list, callback) : UV_EBADF;
}

int writeNativeClose(NativeTransport *transport,
                     uint16_t code,
                     const char *reason,
                     size_t reasonLength) {
    return transport
        ? transport->writeClose(code, reason, reasonLength)
        : UV_EBADF;
}

size_t nativeBufferedAmount(const NativeTransport *transport) {
    return transport ? transport->bufferedAmount() : 0;
}

} // namespace eiowsNode
