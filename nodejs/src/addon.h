#include <node.h>
#include <node_buffer.h>
#include <openssl/ssl.h>
#include <uv.h>

#if NODE_MAJOR_VERSION >= 25
namespace node {
using StartExecutionCallbackWithModule = StartExecutionCallback;
}
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#define NODE_WANT_INTERNALS 1
#include "node/src/crypto/crypto_tls.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using BaseObject = node::BaseObject;
using TLSWrap = node::crypto::TLSWrap;

class TLSWrapSSLGetter : public TLSWrap {
    public:
        void setSSL(const v8::FunctionCallbackInfo<v8::Value> &info){
            v8::Isolate* isolate = info.GetIsolate();
            if (!getSSL()){
                info.GetReturnValue().Set(v8::Null(isolate));
                return;
            }
            SSL* ptr = getSSL()->get();
            info.GetReturnValue().Set(v8::External::New(isolate, ptr));
        }
};
#undef NODE_WANT_INTERNALS

using namespace v8;

eioWS::Hub hub(0);
uv_check_t check;
Persistent<Function> noop;
Persistent<Object> processObject;
Persistent<Function> processNextTick;
bool needsTick = false;
bool checkInitialized = false;

void scheduleTick() {
    needsTick = true;
}

void registerCheck(Isolate *isolate) {
    Local<Context> context = isolate->GetCurrentContext();
    Local<Object> process = context->Global()->Get(
        context,
        String::NewFromUtf8Literal(isolate, "process")
    ).ToLocalChecked().As<Object>();
    processObject.Reset(isolate, process);
    processNextTick.Reset(
        isolate,
        Local<Function>::Cast(
            process->Get(
                context,
                String::NewFromUtf8Literal(isolate, "nextTick")
            ).ToLocalChecked()
        )
    );

    uv_check_init((uv_loop_t *)hub.getLoop(), &check);
    checkInitialized = true;
    check.data = isolate;
    uv_check_start(&check, [](uv_check_t *check) {
        if (!needsTick) {
            return;
        }
        needsTick = false;
        Isolate *isolate = (Isolate *)check->data;
        HandleScope hs(isolate);
        Local<Function>::New(isolate, noop)->Call(isolate->GetCurrentContext(), Null(isolate), 0, nullptr);
    });
    uv_unref((uv_handle_t *)&check);
}

void cleanupAddon(void *) {
    needsTick = false;
    noop.Reset();
    processObject.Reset();
    processNextTick.Reset();
    if (checkInitialized) {
        uv_check_stop(&check);
        if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&check))) {
            uv_close(reinterpret_cast<uv_handle_t *>(&check), nullptr);
        }
        checkInitialized = false;
    }
}

class NativeString {
    char *data;
    size_t length;
    char utf8ValueMemory[sizeof(String::Utf8Value)];
    String::Utf8Value *utf8Value = nullptr;

    public:
    NativeString(Isolate *isolate, const Local<Value> &value) {
        if (value->IsUndefined()) {
            data = nullptr;
            length = 0;
        } else if (value->IsString()) {
            utf8Value = new (utf8ValueMemory) String::Utf8Value(isolate, value);
            data = (**utf8Value);
            length = utf8Value->length();
        } else if (node::Buffer::HasInstance(value)) {
            data = node::Buffer::Data(value);
            length = node::Buffer::Length(value);
        } else if (value->IsTypedArray()) {
            Local<ArrayBufferView> arrayBufferView = Local<ArrayBufferView>::Cast(value);
            auto contents = arrayBufferView->Buffer()->GetBackingStore();
            length = arrayBufferView->ByteLength();
            data = (char *) contents->Data() + arrayBufferView->ByteOffset();
        } else if (value->IsArrayBuffer()) {
            Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(value);
            auto contents = arrayBuffer->GetBackingStore();
            length = contents->ByteLength();
            data = (char *) contents->Data();
        } else if (value->IsSharedArrayBuffer()) {
            Local<SharedArrayBuffer> arrayBuffer = Local<SharedArrayBuffer>::Cast(value);
            auto contents = arrayBuffer->GetBackingStore();
            length = contents->ByteLength();
            data = (char *) contents->Data();
        } else {
            static char empty[] = "";
            data = empty;
            length = 0;
        }
    }

    char *getData() { return data; }

    size_t getLength() const { return length; }

    ~NativeString() {
        if (utf8Value) {
            utf8Value->~Utf8Value();
        }
    }
};

struct GroupData {
    Persistent<Function> connectionHandler, messageHandler, disconnectionHandler;
};

void destroyGroupData(void *data) {
    GroupData *groupData = static_cast<GroupData *>(data);
    groupData->connectionHandler.Reset();
    groupData->messageHandler.Reset();
    groupData->disconnectionHandler.Reset();
    delete groupData;
}

void createGroup(const FunctionCallbackInfo<Value> &args) {
    eioWS::Group *group = hub.createGroup(args[0].As<Integer>()->Value(), args[1].As<Integer>()->Value());
    group->setUserData(new GroupData, destroyGroupData);
    args.GetReturnValue().Set(External::New(args.GetIsolate(), group));
}

inline Local<External> wrapSocket(eioWS::WebSocket *webSocket, Isolate *isolate) {
    return External::New(isolate, webSocket);
}

inline eioWS::WebSocket *unwrapSocket(Local<External> external) {
    return (eioWS::WebSocket *)external->Value();
}

inline Local<Value> wrapMessage(const char *message, size_t length, eioWS::OpCode opCode, Isolate *isolate) {
    if (!message || !length) {
        if (opCode == eioWS::OpCode::BINARY) {
            return node::Buffer::Copy(isolate, "", 0).ToLocalChecked();
        }
        return String::Empty(isolate);
    }

    if (opCode == eioWS::OpCode::BINARY) {
        return node::Buffer::Copy(isolate, (char *)message, length).ToLocalChecked();
    } else {
        return String::NewFromUtf8(isolate, message, NewStringType::kNormal, length).ToLocalChecked();
    }
}

inline Persistent<Value> *getUserDataRef(eioWS::WebSocket *webSocket) {
    return static_cast<Persistent<Value> *>(webSocket->getUserData());
}

inline Local<Value> getDataV8(eioWS::WebSocket *webSocket, Isolate *isolate) {
    Persistent<Value> *userData = getUserDataRef(webSocket);
    return userData ? Local<Value>::New(isolate, *userData) : Local<Value>::Cast(Undefined(isolate));
}

void clearUserData(const FunctionCallbackInfo<Value> &args) {
    eioWS::WebSocket *webSocket = unwrapSocket(args[0].As<External>());
    Persistent<Value> *userData = getUserDataRef(webSocket);
    if (userData) {
        userData->Reset();
        delete userData;
        webSocket->setUserData(nullptr);
    }
}

void setUserData(const FunctionCallbackInfo<Value> &args) {
    eioWS::WebSocket *webSocket = unwrapSocket(args[0].As<External>());
    Persistent<Value> *userData = getUserDataRef(webSocket);
    if (userData) {
        userData->Reset(args.GetIsolate(), args[1]);
    } else {
        webSocket->setUserData(new Persistent<Value>(args.GetIsolate(), args[1]));
    }
}

void getAddress(const FunctionCallbackInfo<Value> &args) {
    typename eioWS::WebSocket::Address address = unwrapSocket(args[0].As<External>())->getAddress();
    Isolate *isolate = args.GetIsolate();
    Local<Array> array = Array::New(isolate, 3);
    array->Set(isolate->GetCurrentContext(), 0, Integer::New(isolate, address.port));
    array->Set(isolate->GetCurrentContext(), 1, String::NewFromUtf8(isolate, address.address, NewStringType::kNormal).ToLocalChecked());
    array->Set(isolate->GetCurrentContext(), 2, String::NewFromUtf8(isolate, address.family,  NewStringType::kNormal).ToLocalChecked());
    args.GetReturnValue().Set(array);
}

uv_handle_t *getTcpHandle(void *handleWrap) {
    volatile char *memory = (volatile char *)handleWrap;
    for (volatile uv_handle_t *tcpHandle = (volatile uv_handle_t *)memory;
            tcpHandle->type != UV_TCP || tcpHandle->data != handleWrap ||
            tcpHandle->loop != uv_default_loop();
            tcpHandle = (volatile uv_handle_t *)memory) {
        memory++;
    }
    return (uv_handle_t *)memory;
}

struct SendCallbackData {
    Persistent<Function> jsCallback;
    Isolate *isolate;
};

void sendCallback(eioWS::WebSocket *, void *data, bool cancelled, void *) {
    SendCallbackData *sc = static_cast<SendCallbackData *>(data);
    HandleScope hs(sc->isolate);
    Local<Context> context = sc->isolate->GetCurrentContext();
    Local<Object> process = Local<Object>::New(sc->isolate, processObject);
    Local<Function> nextTick = Local<Function>::New(sc->isolate, processNextTick);
    Local<Value> argv[] = {
        Local<Function>::New(sc->isolate, sc->jsCallback),
        cancelled ? Exception::Error(String::NewFromUtf8Literal(sc->isolate, "send cancelled")) : Undefined(sc->isolate)
    };
    nextTick->Call(context, process, cancelled ? 2 : 1, argv);
    scheduleTick();

    sc->jsCallback.Reset();
    delete sc;
}

void send(const FunctionCallbackInfo<Value> &args) {
    eioWS::OpCode opCode = (eioWS::OpCode)args[2].As<Integer>()->Value();
    NativeString nativeString(args.GetIsolate(), args[1]);

    SendCallbackData *sc = nullptr;
    void (*callback)(eioWS::WebSocket *, void *, bool, void *) = nullptr;

    if (args[3]->IsFunction()) {
        callback = sendCallback;
        sc = new SendCallbackData;
        sc->jsCallback.Reset(args.GetIsolate(), Local<Function>::Cast(args[3]));
        sc->isolate = args.GetIsolate();
    }

    bool compress = args[4].As<Boolean>()->Value();
    unwrapSocket(args[0].As<External>())->send(nativeString.getData(), nativeString.getLength(), opCode, callback, sc, compress);
}

struct Ticket {
    uv_os_sock_t fd;
    SSL *ssl;
};

void releaseTicket(Ticket *ticket) {
    if (!ticket) {
        return;
    }
    if (ticket->fd != INVALID_SOCKET) {
        uS::Context::closeSocket(ticket->fd);
        ticket->fd = INVALID_SOCKET;
    }
    if (ticket->ssl) {
        SSL_free(ticket->ssl);
        ticket->ssl = nullptr;
    }
    delete ticket;
}

void upgrade(const FunctionCallbackInfo<Value> &args) {
    eioWS::Group *serverGroup = (eioWS::Group *)args[0].As<External>()->Value();
    Ticket *ticket = static_cast<Ticket *>(args[1].As<External>()->Value());
    Isolate *isolate = args.GetIsolate();
    NativeString secKey(isolate, args[2]);
    NativeString extensions(isolate, args[3]);
    NativeString subprotocol(isolate, args[4]);

    // todo: move this check into core!
    if (ticket->fd != INVALID_SOCKET) {
        hub.upgrade(ticket->fd, secKey.getData(), ticket->ssl, extensions.getData(), extensions.getLength(), subprotocol.getData(), subprotocol.getLength(), serverGroup);
        ticket->fd = INVALID_SOCKET;
        ticket->ssl = nullptr;
        args.GetReturnValue().Set(True(isolate));
    } else {
        releaseTicket(ticket);
        args.GetReturnValue().Set(False(isolate));
        return;
    }
    delete ticket;
}

void destroyTicket(const FunctionCallbackInfo<Value> &args) {
    if (args.Length() && args[0]->IsExternal()) {
        releaseTicket(static_cast<Ticket *>(args[0].As<External>()->Value()));
    }
}

void transfer(const FunctionCallbackInfo<Value> &args) {
    // (_handle.fd OR _handle), SSL
    uv_handle_t *handle = nullptr;
    Ticket *ticket = new Ticket;
    if (args[0]->IsObject()) {
        Local<Context> context = args.GetIsolate()->GetCurrentContext();
        uv_fileno((handle = getTcpHandle( args[0]->ToObject(context).ToLocalChecked()->GetAlignedPointerFromInternalField(0))), (uv_os_fd_t *)&ticket->fd);
    } else {
        ticket->fd = args[0].As<Integer>()->Value();
    }

    ticket->fd = dup(ticket->fd);
    ticket->ssl = nullptr;
    if (args[1]->IsExternal()) {
        ticket->ssl = (SSL *)args[1].As<External>()->Value();
        SSL_up_ref(ticket->ssl);
    }

    // uv_close calls shutdown if not set on Windows
    if (handle) {
        // UV_HANDLE_SHARED_TCP_SOCKET
        handle->flags |= 0x40000000;
    }

    args.GetReturnValue().Set(External::New(args.GetIsolate(), ticket));
}

void onConnection(const FunctionCallbackInfo<Value> &args) {
    eioWS::Group *group = (eioWS::Group *)args[0].As<External>()->Value();
    GroupData *groupData = static_cast<GroupData *>(group->getUserData());

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *connectionCallback = &groupData->connectionHandler;
    connectionCallback->Reset(isolate, Local<Function>::Cast(args[1]));
    group->onConnection([isolate, connectionCallback](eioWS::WebSocket *webSocket) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapSocket(webSocket, isolate)};
        Local<Function>::New(isolate, *connectionCallback)->Call(isolate->GetCurrentContext(), Null(isolate), 1, argv);
        scheduleTick();
    });
}

void onMessage(const FunctionCallbackInfo<Value> &args) {
    eioWS::Group *group = (eioWS::Group *)args[0].As<External>()->Value();
    GroupData *groupData = static_cast<GroupData *>(group->getUserData());

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *messageCallback = &groupData->messageHandler;

    messageCallback->Reset(isolate, Local<Function>::Cast(args[1]));
    group->onMessage([isolate, messageCallback](eioWS::WebSocket *webSocket, const char *message, size_t length, eioWS::OpCode opCode) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {
            wrapMessage(message, length, opCode, isolate),
            getDataV8(webSocket, isolate),
            Boolean::New(isolate, opCode == eioWS::OpCode::BINARY)
        };
        Local<Function>::New(isolate, *messageCallback)->Call(isolate->GetCurrentContext(), Null(isolate), 3, argv);
        scheduleTick();
    });
}

void onDisconnection(const FunctionCallbackInfo<Value> &args) {
    eioWS::Group *group = (eioWS::Group *)args[0].As<External>()->Value();
    GroupData *groupData = static_cast<GroupData *>(group->getUserData());

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *disconnectionCallback = &groupData->disconnectionHandler;
    disconnectionCallback->Reset(isolate, Local<Function>::Cast(args[1]));

    group->onDisconnection([isolate, disconnectionCallback](eioWS::WebSocket *webSocket, int code, char *message, size_t length) {
        HandleScope hs(isolate);
        Local<Value> argv[] = {
        wrapSocket(webSocket, isolate), Integer::New(isolate, code),
        wrapMessage(message, length, eioWS::OpCode::CLOSE, isolate),
        getDataV8(webSocket, isolate)};
        Local<Function>::New(isolate, *disconnectionCallback)->Call(isolate->GetCurrentContext(), Null(isolate), 4, argv);
        scheduleTick();
    });
}

void closeSocket(const FunctionCallbackInfo<Value> &args) {
    int code = args[1]->IsNumber() ? args[1].As<Integer>()->Value() : 1000;
    NativeString nativeString(args.GetIsolate(), args[2]);
    unwrapSocket(args[0].As<External>())->close(code, nativeString.getData(), nativeString.getLength());
}

void closeGroup(const FunctionCallbackInfo<Value> &args) {
    int code = args[1]->IsNumber() ? args[1].As<Integer>()->Value() : 1000;
    NativeString nativeString(args.GetIsolate(), args[2]);
    eioWS::Group *group = (eioWS::Group *)args[0].As<External>()->Value();
    group->close(code, nativeString.getData(), nativeString.getLength());
    group->markForDeletion();
}

void getSSLContext(const FunctionCallbackInfo<Value> &args) {
    Isolate* isolate = args.GetIsolate();
    if(args.Length() < 1 || !args[0]->IsObject()){
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Error: One object expected", NewStringType::kNormal).ToLocalChecked()));
        return;
    }
    Local<Context> context = isolate->GetCurrentContext();
    Local<Object> obj = args[0]->ToObject(context).ToLocalChecked();
    TLSWrapSSLGetter* tw;
    ASSIGN_OR_RETURN_UNWRAP(&tw, obj);
    tw->setSSL(args);
}

    void setNoop(const FunctionCallbackInfo<Value> &args) {
    noop.Reset(args.GetIsolate(), Local<Function>::Cast(args[0]));
}

struct Namespace {
    Local<Object> object;
    Namespace(Isolate *isolate) {
        object = Object::New(isolate);
        NODE_SET_METHOD(object, "send", send);
        NODE_SET_METHOD(object, "close", closeSocket);

        Local<Object> group = Object::New(isolate);
        NODE_SET_METHOD(group, "onConnection", onConnection);
        NODE_SET_METHOD(group, "onMessage", onMessage);
        NODE_SET_METHOD(group, "onDisconnection", onDisconnection);

        NODE_SET_METHOD(group, "create", createGroup);
        NODE_SET_METHOD(group, "close", closeGroup);

        object->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "group", NewStringType::kNormal).ToLocalChecked(), group);
    }
};
