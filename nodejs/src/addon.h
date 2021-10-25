#include <node.h>
#include <node_buffer.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <uv.h>
#include <cstring>

#if NODE_MAJOR_VERSION>=10
#define NODE_WANT_INTERNALS 1
#if NODE_MAJOR_VERSION==10
#include "node_10_headers/tls_wrap.h"
#endif
#if NODE_MAJOR_VERSION==12
#if NODE_MINOR_VERSION>=18
#include "node_12.18_headers/tls_wrap.h"
#include "node_12.18_headers/base_object-inl.h"
#else
#include "node_12.16_headers/tls_wrap.h"
#include "node_12.16_headers/base_object-inl.h"
#endif
#endif
#if (NODE_MAJOR_VERSION==13 && NODE_MINOR_VERSION>=14)
#include "node_13_headers/tls_wrap.h"
#include "node_13_headers/base_object-inl.h"
#endif
#if (NODE_MAJOR_VERSION==14 && NODE_MINOR_VERSION>=13)
#include "node_14.13_headers/tls_wrap.h"
#include "node_14.13_headers/base_object-inl.h"
#elif (NODE_MAJOR_VERSION==14 && NODE_MINOR_VERSION>=4)
#include "node_14_headers/tls_wrap.h"
#include "node_14_headers/base_object-inl.h"
#endif
#if NODE_MAJOR_VERSION==15
#include "node_15_headers/crypto_tls.h"
#endif
#if NODE_MAJOR_VERSION==16
#include "node_16_headers/crypto/crypto_tls.h"
#endif
#if NODE_MAJOR_VERSION==17
#include "node_17_headers/crypto/crypto_tls.h"
#endif

using BaseObject = node::BaseObject;
#if NODE_MAJOR_VERSION>=15
using TLSWrap = node::crypto::TLSWrap;
#else
using TLSWrap = node::TLSWrap;
#endif

class TLSWrapSSLGetter : public TLSWrap {
    public:
        void setSSL(const v8::FunctionCallbackInfo<v8::Value> &info){
            v8::Isolate* isolate = info.GetIsolate();
#if NODE_MAJOR_VERSION>=15
            if (!getSSL()){
#else
            if (!ssl_){
#endif
                info.GetReturnValue().Set(v8::Null(isolate));
                return;
            }
#if NODE_MAJOR_VERSION>=15
            SSL* ptr = getSSL()->get();
#else
            SSL* ptr = ssl_.get();
#endif
            info.GetReturnValue().Set(v8::External::New(isolate, ptr));
        }
};

#if defined(_MSC_VER)
  #if NODE_MAJOR_VERSION>10
    [[noreturn]] void node::Assert(const node::AssertionInfo& info) {
      char name[1024];
      char title[1024] = "Node.js";
      uv_get_process_title(title, sizeof(title));
      snprintf(name, sizeof(name), "%s[%d]", title, uv_os_getpid());
      fprintf(stderr, "%s: Assertion failed.\n", name);
      fflush(stderr);
      ABORT_NO_BACKTRACE();
    }
  #else
    [[noreturn]] void node::Assert(const char* const (*args)[4]) {
      auto filename = (*args)[0];
      auto linenum = (*args)[1];
      auto message = (*args)[2];
      auto function = (*args)[3];
      char name[1024];
      char title[1024] = "Node.js";
      uv_get_process_title(title, sizeof(title));
      snprintf(name, sizeof(name), "%s[%d]", title, uv_os_getpid());
      fprintf(stderr, "%s: %s:%s:%s%s Assertion `%s' failed.\n",
              name, filename, linenum, function, *function ? ":" : "", message);
      fflush(stderr);
      ABORT_NO_BACKTRACE();
    }
  #endif
#endif
#undef NODE_WANT_INTERNALS
#endif

using namespace std;
using namespace v8;

eioWS::Hub hub(0);
uv_check_t check;
Persistent<Function> noop;

void registerCheck(Isolate *isolate) {
    uv_check_init((uv_loop_t *)hub.getLoop(), &check);
    check.data = isolate;
    uv_check_start(&check, [](uv_check_t *check) {
        Isolate *isolate = (Isolate *)check->data;
        HandleScope hs(isolate);
        // TODO: Check if we can use new callbback
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), Local<Function>::New(isolate, noop), 0, nullptr);
    });
    uv_unref((uv_handle_t *)&check);
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
#if NODE_MAJOR_VERSION>=17
            auto contents = arrayBufferView->Buffer()->GetBackingStore();
            length = arrayBufferView->ByteLength();
            data = (char *) contents->Data() + arrayBufferView->ByteOffset();
#else
            ArrayBuffer::Contents contents = arrayBufferView->Buffer()->GetContents();
            length = contents.ByteLength();
            data = (char *)contents.Data();
#endif
        } else if (value->IsArrayBuffer()) {
            Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(value);
#if NODE_MAJOR_VERSION>=17
            auto contents = arrayBuffer->GetBackingStore();
            length = contents->ByteLength();
            data = (char *) contents->Data();
#else
            ArrayBuffer::Contents contents = arrayBuffer->GetContents();
            length = contents.ByteLength();
            data = (char *)contents.Data();
#endif
        } else if (value->IsSharedArrayBuffer()) {
            Local<SharedArrayBuffer> arrayBuffer = Local<SharedArrayBuffer>::Cast(value);
#if NODE_MAJOR_VERSION>=17
            auto contents = arrayBuffer->GetBackingStore();
            length = contents->ByteLength();
            data = (char *) contents->Data();
#else
            SharedArrayBuffer::Contents contents = arrayBuffer->GetContents();
            length = contents.ByteLength();
            data = (char *)contents.Data();
#endif
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
    int size = 0;
};

void createGroup(const FunctionCallbackInfo<Value> &args) {
    eioWS::Group *group = hub.createGroup(args[0].As<Integer>()->Value(), args[1].As<Integer>()->Value());
    group->setUserData(new GroupData);
    args.GetReturnValue().Set(External::New(args.GetIsolate(), group));
}

inline Local<External> wrapSocket(eioWS::WebSocket *webSocket, Isolate *isolate) {
    return External::New(isolate, webSocket);
}

inline eioWS::WebSocket *unwrapSocket(Local<External> external) {
    return (eioWS::WebSocket *)external->Value();
}

inline Local<Value> wrapMessage(const char *message, size_t length, eioWS::OpCode opCode, Isolate *isolate) {
    if (opCode == eioWS::OpCode::BINARY) {
        return node::Buffer::Copy(isolate, (char *)message, length).ToLocalChecked();
    } else {
        return String::NewFromUtf8(isolate, message, NewStringType::kNormal, length).ToLocalChecked();
    }
}

inline Local<Value> getDataV8(eioWS::WebSocket *webSocket, Isolate *isolate) {
    return webSocket->getUserData() ? Local<Value>::New(isolate, *(Persistent<Value> *)webSocket->getUserData()) : Local<Value>::Cast(Undefined(isolate));
}

void getUserData(const FunctionCallbackInfo<Value> &args) {
    args.GetReturnValue().Set(getDataV8(unwrapSocket(args[0].As<External>()), args.GetIsolate()));
}

void clearUserData(const FunctionCallbackInfo<Value> &args) {
    eioWS::WebSocket *webSocket = unwrapSocket(args[0].As<External>());
    ((Persistent<Value> *)webSocket->getUserData())->Reset();
    delete (Persistent<Value> *)webSocket->getUserData();
}

void setUserData(const FunctionCallbackInfo<Value> &args) {
    eioWS::WebSocket *webSocket = unwrapSocket(args[0].As<External>());
    if (webSocket->getUserData()) {
        ((Persistent<Value> *)webSocket->getUserData())->Reset(args.GetIsolate(), args[1]);
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

void sendCallback(eioWS::WebSocket *webSocket, void *data, bool cancelled, void *reserved) {
    SendCallbackData *sc = static_cast<SendCallbackData *>(data);
    if (!cancelled) {
        HandleScope hs(sc->isolate);
        Local<Function>::New(sc->isolate, sc->jsCallback)->Call(sc->isolate->GetCurrentContext(), Null(sc->isolate), 0, nullptr);
    }
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
    } else {
        if (ticket->ssl) {
            SSL_free(ticket->ssl);
        }
    }
    delete ticket;
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
    group->onConnection([isolate, connectionCallback, groupData](eioWS::WebSocket *webSocket) {
        groupData->size++;
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapSocket(webSocket, isolate)};
        Local<Function>::New(isolate, *connectionCallback)->Call(isolate->GetCurrentContext(), Null(isolate), 1, argv);
    });
}

void onMessage(const FunctionCallbackInfo<Value> &args) {
    eioWS::Group *group = (eioWS::Group *)args[0].As<External>()->Value();
    GroupData *groupData = static_cast<GroupData *>(group->getUserData());

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *messageCallback = &groupData->messageHandler;

    messageCallback->Reset(isolate, Local<Function>::Cast(args[1]));
    group->onMessage([isolate, messageCallback, group](eioWS::WebSocket *webSocket, const char *message, size_t length, eioWS::OpCode opCode) {
        if(length != 1 || message[0] != 65) {
            HandleScope hs(isolate);
            Local<Value> argv[] = {wrapMessage(message, length, opCode, isolate),
            getDataV8(webSocket, isolate)};
            Local<Function>::New(isolate, *messageCallback)->Call(isolate->GetCurrentContext(), Null(isolate), 2, argv);
        }
    });
}

void onDisconnection(const FunctionCallbackInfo<Value> &args) {
    eioWS::Group *group = (eioWS::Group *)args[0].As<External>()->Value();
    GroupData *groupData = static_cast<GroupData *>(group->getUserData());

    Isolate *isolate = args.GetIsolate();
    Persistent<Function> *disconnectionCallback = &groupData->disconnectionHandler;
    disconnectionCallback->Reset(isolate, Local<Function>::Cast(args[1]));

    group->onDisconnection([isolate, disconnectionCallback, groupData]( eioWS::WebSocket *webSocket, int code, char *message, size_t length) {
        groupData->size--;
        HandleScope hs(isolate);
        Local<Value> argv[] = {
        wrapSocket(webSocket, isolate), Integer::New(isolate, code),
        wrapMessage(message, length, eioWS::OpCode::CLOSE, isolate),
        getDataV8(webSocket, isolate)};
        Local<Function>::New(isolate, *disconnectionCallback)->Call(isolate->GetCurrentContext(), Null(isolate), 4, argv);
    });
}

void closeSocket(const FunctionCallbackInfo<Value> &args) {
    NativeString nativeString(args.GetIsolate(), args[2]);
    unwrapSocket(args[0].As<External>())->close(args[1].As<Integer>()->Value(), nativeString.getData(), nativeString.getLength());
}

void closeGroup(const FunctionCallbackInfo<Value> &args) {
    NativeString nativeString(args.GetIsolate(), args[2]);
    eioWS::Group *group = (eioWS::Group *)args[0].As<External>()->Value();
    group->close(args[1].As<Integer>()->Value(), nativeString.getData(), nativeString.getLength());
}

void getSSLContext(const FunctionCallbackInfo<Value> &args) {
    Isolate* isolate = args.GetIsolate();
    if(args.Length() < 1 || !args[0]->IsObject()){
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Error: One object expected", NewStringType::kNormal).ToLocalChecked()));
        return;
    }
    Local<Context> context = isolate->GetCurrentContext();
    Local<Object> obj = args[0]->ToObject(context).ToLocalChecked();
#if NODE_MAJOR_VERSION < 10
    Local<Value> ext = obj->Get(String::NewFromUtf8(isolate, "_external"));
    args.GetReturnValue().Set(ext);
#else
    TLSWrapSSLGetter* tw;
    ASSIGN_OR_RETURN_UNWRAP(&tw, obj);
    tw->setSSL(args);
#endif
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
