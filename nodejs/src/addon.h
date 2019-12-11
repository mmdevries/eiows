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
  #include "node_12_headers/tls_wrap.h"
#endif
using BaseObject = node::BaseObject;
using TLSWrap = node::TLSWrap;
class TLSWrapSSLGetter : public node::TLSWrap {
public:
    void setSSL(const v8::FunctionCallbackInfo<v8::Value> &info){
        v8::Isolate* isolate = info.GetIsolate();
        if (!ssl_){
            info.GetReturnValue().Set(v8::Null(isolate));
            return;
        }
        SSL* ptr = ssl_.get();
        v8::Local<v8::External> ext = v8::External::New(isolate, ptr);
        info.GetReturnValue().Set(ext);
    }
};
 #undef NODE_WANT_INTERNALS
#endif

using namespace std;
using namespace v8;

uWS::Hub hub(0, true);
uv_check_t check;
Persistent<Function> noop;

void registerCheck(Isolate *isolate) {
  uv_check_init((uv_loop_t *)hub.getLoop(), &check);
  check.data = isolate;
  uv_check_start(&check, [](uv_check_t *check) {
    Isolate *isolate = (Isolate *)check->data;
    HandleScope hs(isolate);
    node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(),
                       Local<Function>::New(isolate, noop), 0, nullptr);
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
      Local<ArrayBufferView> arrayBufferView =
          Local<ArrayBufferView>::Cast(value);
      ArrayBuffer::Contents contents = arrayBufferView->Buffer()->GetContents();
      length = contents.ByteLength();
      data = (char *)contents.Data();
    } else if (value->IsArrayBuffer()) {
      Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(value);
      ArrayBuffer::Contents contents = arrayBuffer->GetContents();
      length = contents.ByteLength();
      data = (char *)contents.Data();
    } else {
      static char empty[] = "";
      data = empty;
      length = 0;
    }
  }

  char *getData() { return data; }
  size_t getLength() { return length; }
  ~NativeString() {
    if (utf8Value) {
      utf8Value->~Utf8Value();
    }
  }
};

struct GroupData {
  Persistent<Function> connectionHandler, messageHandler, disconnectionHandler,
      pingHandler, pongHandler, errorHandler, httpRequestHandler,
      httpUpgradeHandler, httpCancelledRequestCallback;
  int size = 0;
};

template <bool isServer>
void createGroup(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<isServer> *group = hub.createGroup<isServer>(
      args[0].As<Integer>()->Value(), args[1].As<Integer>()->Value());
  group->setUserData(new GroupData);
  args.GetReturnValue().Set(External::New(args.GetIsolate(), group));
}

// TODO: This is never called by the js wrapper,
//       not sure if this is a potential memory leak.
template <bool isServer>
void deleteGroup(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<isServer> *group =
      (uWS::Group<isServer> *)args[0].As<External>()->Value();
  delete (GroupData *)group->getUserData();
  delete group;
}

template <bool isServer>
inline Local<External> wrapSocket(uWS::WebSocket<isServer> *webSocket,
                                  Isolate *isolate) {
  return External::New(isolate, webSocket);
}

template <bool isServer>
inline uWS::WebSocket<isServer> *unwrapSocket(Local<External> external) {
  return (uWS::WebSocket<isServer> *)external->Value();
}

inline Local<Value> wrapMessage(const char *message, size_t length,
                                uWS::OpCode opCode, Isolate *isolate) {
  if (opCode == uWS::OpCode::BINARY) {
      return  (Local<Value>)ArrayBuffer::New(isolate, (char *)message, length);
  } else {
      MaybeLocal<String> messageSt = String::NewFromUtf8(isolate, message, NewStringType::kNormal, length);
      return (Local<Value>)messageSt.ToLocalChecked();
  }
}

template <bool isServer>
inline Local<Value> getDataV8(uWS::WebSocket<isServer> *webSocket,
                              Isolate *isolate) {
  return webSocket->getUserData()
             ? Local<Value>::New(isolate,
                                 *(Persistent<Value> *)webSocket->getUserData())
             : Local<Value>::Cast(Undefined(isolate));
}

template <bool isServer>
void getUserData(const FunctionCallbackInfo<Value> &args) {
  args.GetReturnValue().Set(getDataV8(
      unwrapSocket<isServer>(args[0].As<External>()), args.GetIsolate()));
}

template <bool isServer>
void clearUserData(const FunctionCallbackInfo<Value> &args) {
  uWS::WebSocket<isServer> *webSocket =
      unwrapSocket<isServer>(args[0].As<External>());
  ((Persistent<Value> *)webSocket->getUserData())->Reset();
  delete (Persistent<Value> *)webSocket->getUserData();
}

template <bool isServer>
void setUserData(const FunctionCallbackInfo<Value> &args) {
  uWS::WebSocket<isServer> *webSocket =
      unwrapSocket<isServer>(args[0].As<External>());
  if (webSocket->getUserData()) {
    ((Persistent<Value> *)webSocket->getUserData())
        ->Reset(args.GetIsolate(), args[1]);
  } else {
    webSocket->setUserData(new Persistent<Value>(args.GetIsolate(), args[1]));
  }
}

template <bool isServer>
void getAddress(const FunctionCallbackInfo<Value> &args) {
  typename uWS::WebSocket<isServer>::Address address =
      unwrapSocket<isServer>(args[0].As<External>())->getAddress();
  Isolate *isolate = args.GetIsolate();
  MaybeLocal<String> addressSt = String::NewFromUtf8(isolate, address.address, NewStringType::kNormal);
  MaybeLocal<String> familySt = String::NewFromUtf8(isolate, address.family, NewStringType::kNormal);
  Local<Array> array = Array::New(isolate, 3);
  array->Set(0, Integer::New(isolate, address.port));
  array->Set(1, addressSt.ToLocalChecked());
  array->Set(2, familySt.ToLocalChecked());
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

template <bool isServer>
void sendCallback(uWS::WebSocket<isServer> *webSocket, void *data,
                  bool cancelled, void *reserved) {
  SendCallbackData *sc = (SendCallbackData *)data;
  if (!cancelled) {
    HandleScope hs(sc->isolate);
    node::MakeCallback(sc->isolate, sc->isolate->GetCurrentContext()->Global(),
                       Local<Function>::New(sc->isolate, sc->jsCallback), 0,
                       nullptr);
  }
  sc->jsCallback.Reset();
  delete sc;
}

template <bool isServer>
void send(const FunctionCallbackInfo<Value> &args) {
  uWS::OpCode opCode = (uWS::OpCode)args[2].As<Integer>()->Value();
  NativeString nativeString(args.GetIsolate(), args[1]);

  SendCallbackData *sc = nullptr;
  void (*callback)(uWS::WebSocket<isServer> *, void *, bool, void *) = nullptr;

  if (args[3]->IsFunction()) {
    callback = sendCallback;
    sc = new SendCallbackData;
    sc->jsCallback.Reset(args.GetIsolate(), Local<Function>::Cast(args[3]));
    sc->isolate = args.GetIsolate();
  }

  bool compress = args[4].As<Boolean>()->Value();

  unwrapSocket<isServer>(args[0].As<External>())
      ->send(nativeString.getData(), nativeString.getLength(), opCode, callback,
             sc, compress);
}

void connect(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<uWS::CLIENT> *clientGroup =
      (uWS::Group<uWS::CLIENT> *)args[0].As<External>()->Value();
  NativeString uri(args.GetIsolate(), args[1]);
  hub.connect(std::string(uri.getData(), uri.getLength()),
              new Persistent<Value>(args.GetIsolate(), args[2]), {}, 5000,
              clientGroup);
}

struct Ticket {
  uv_os_sock_t fd;
  SSL *ssl;
};

void upgrade(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<uWS::SERVER> *serverGroup =
      (uWS::Group<uWS::SERVER> *)args[0].As<External>()->Value();
  Ticket *ticket = (Ticket *)args[1].As<External>()->Value();
  Isolate *isolate = args.GetIsolate();
  NativeString secKey(isolate, args[2]);
  NativeString extensions(isolate, args[3]);
  NativeString subprotocol(isolate, args[4]);

  // todo: move this check into core!
  if (ticket->fd != INVALID_SOCKET) {
    hub.upgrade(ticket->fd, secKey.getData(), ticket->ssl, extensions.getData(),
                extensions.getLength(), subprotocol.getData(),
                subprotocol.getLength(), serverGroup);
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
    Isolate* isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();

    uv_fileno((handle = getTcpHandle(
                   args[0]->ToObject(context).ToLocalChecked()->GetAlignedPointerFromInternalField(0))),
              (uv_os_fd_t *)&ticket->fd);
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

template <bool isServer>
void onConnection(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<isServer> *group =
      (uWS::Group<isServer> *)args[0].As<External>()->Value();
  GroupData *groupData = (GroupData *)group->getUserData();

  Isolate *isolate = args.GetIsolate();
  Persistent<Function> *connectionCallback = &groupData->connectionHandler;
  connectionCallback->Reset(isolate, Local<Function>::Cast(args[1]));
  group->onConnection(
      [isolate, connectionCallback, groupData](
          uWS::WebSocket<isServer> *webSocket, uWS::HttpRequest req) {
        groupData->size++;
        HandleScope hs(isolate);
        Local<Value> argv[] = {wrapSocket(webSocket, isolate)};
        node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(),
                           Local<Function>::New(isolate, *connectionCallback),
                           1, argv);
      });
}

template <bool isServer>
void onMessage(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<isServer> *group =
      (uWS::Group<isServer> *)args[0].As<External>()->Value();
  GroupData *groupData = (GroupData *)group->getUserData();

  Isolate *isolate = args.GetIsolate();
  Persistent<Function> *messageCallback = &groupData->messageHandler;

  messageCallback->Reset(isolate, Local<Function>::Cast(args[1]));
  group->onMessage([isolate, messageCallback, group](
                       uWS::WebSocket<isServer> *webSocket, const char *message,
                       size_t length, uWS::OpCode opCode) {
    if(length == 1 && message[0] == 65) {
      // emit pong event is we get pong from the client
      group->pongHandler(webSocket, nullptr, 0);
    } else {
      HandleScope hs(isolate);
      Local<Value> argv[] = {wrapMessage(message, length, opCode, isolate),
                            getDataV8(webSocket, isolate)};
      Local<Function>::New(isolate, *messageCallback)
          ->Call(isolate->GetCurrentContext(), Null(isolate), 2, argv);
    }
  });
}

template <bool isServer>
void onPing(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<isServer> *group =
      (uWS::Group<isServer> *)args[0].As<External>()->Value();
  GroupData *groupData = (GroupData *)group->getUserData();

  Isolate *isolate = args.GetIsolate();
  Persistent<Function> *pingCallback = &groupData->pingHandler;
  pingCallback->Reset(isolate, Local<Function>::Cast(args[1]));
  group->onPing([isolate, pingCallback](uWS::WebSocket<isServer> *webSocket,
                                        const char *message, size_t length) {
    HandleScope hs(isolate);
    Local<Value> argv[] = {
        wrapMessage(message, length, uWS::OpCode::PING, isolate),
        getDataV8(webSocket, isolate)};
    node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(),
                       Local<Function>::New(isolate, *pingCallback), 2, argv);
  });
}

template <bool isServer>
void onPong(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<isServer> *group =
      (uWS::Group<isServer> *)args[0].As<External>()->Value();
  GroupData *groupData = (GroupData *)group->getUserData();

  Isolate *isolate = args.GetIsolate();
  Persistent<Function> *pongCallback = &groupData->pongHandler;
  pongCallback->Reset(isolate, Local<Function>::Cast(args[1]));
  group->onPong([isolate, pongCallback](uWS::WebSocket<isServer> *webSocket,
                                        const char *message, size_t length) {
    HandleScope hs(isolate);
    Local<Value> argv[] = {
        wrapMessage(message, length, uWS::OpCode::PONG, isolate),
        getDataV8(webSocket, isolate)};
    node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(),
                       Local<Function>::New(isolate, *pongCallback), 2, argv);
  });
}

template <bool isServer>
void onDisconnection(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<isServer> *group =
      (uWS::Group<isServer> *)args[0].As<External>()->Value();
  GroupData *groupData = (GroupData *)group->getUserData();

  Isolate *isolate = args.GetIsolate();
  Persistent<Function> *disconnectionCallback =
      &groupData->disconnectionHandler;
  disconnectionCallback->Reset(isolate, Local<Function>::Cast(args[1]));

  group->onDisconnection([isolate, disconnectionCallback, groupData](
                             uWS::WebSocket<isServer> *webSocket, int code,
                             char *message, size_t length) {
    groupData->size--;
    HandleScope hs(isolate);
    Local<Value> argv[] = {
        wrapSocket(webSocket, isolate), Integer::New(isolate, code),
        wrapMessage(message, length, uWS::OpCode::CLOSE, isolate),
        getDataV8(webSocket, isolate)};
    node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(),
                       Local<Function>::New(isolate, *disconnectionCallback), 4,
                       argv);
  });
}

void onError(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<uWS::CLIENT> *group =
      (uWS::Group<uWS::CLIENT> *)args[0].As<External>()->Value();
  GroupData *groupData = (GroupData *)group->getUserData();

  Isolate *isolate = args.GetIsolate();
  Persistent<Function> *errorCallback = &groupData->errorHandler;
  errorCallback->Reset(isolate, Local<Function>::Cast(args[1]));

  group->onError([isolate, errorCallback](void *user) {
    HandleScope hs(isolate);
    Local<Value> argv[] = {
        Local<Value>::New(isolate, *(Persistent<Value> *)user)};
    node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(),
                       Local<Function>::New(isolate, *errorCallback), 1, argv);

    ((Persistent<Value> *)user)->Reset();
    delete (Persistent<Value> *)user;
  });
}

template <bool isServer>
void closeSocket(const FunctionCallbackInfo<Value> &args) {
  NativeString nativeString(args.GetIsolate(), args[2]);
  unwrapSocket<isServer>(args[0].As<External>())
      ->close(args[1].As<Integer>()->Value(), nativeString.getData(),
              nativeString.getLength());
}

template <bool isServer>
void terminateSocket(const FunctionCallbackInfo<Value> &args) {
  unwrapSocket<isServer>(args[0].As<External>())->terminate();
}

template <bool isServer>
void closeGroup(const FunctionCallbackInfo<Value> &args) {
  NativeString nativeString(args.GetIsolate(), args[2]);
  uWS::Group<isServer> *group =
      (uWS::Group<isServer> *)args[0].As<External>()->Value();
  group->close(args[1].As<Integer>()->Value(), nativeString.getData(),
               nativeString.getLength());
}

template <bool isServer>
void terminateGroup(const FunctionCallbackInfo<Value> &args) {
  ((uWS::Group<isServer> *)args[0].As<External>()->Value())->terminate();
}

template <bool isServer>
void broadcast(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<isServer> *group =
      (uWS::Group<isServer> *)args[0].As<External>()->Value();
  uWS::OpCode opCode =
      args[2].As<Boolean>()->Value() ? uWS::OpCode::BINARY : uWS::OpCode::TEXT;
  NativeString nativeString(args.GetIsolate(), args[1]);
  group->broadcast(nativeString.getData(), nativeString.getLength(), opCode, false);
}

template <bool isServer>
void prepareMessage(const FunctionCallbackInfo<Value> &args) {
  uWS::OpCode opCode = (uWS::OpCode)args[1].As<Integer>()->Value();
  NativeString nativeString(args.GetIsolate(), args[0]);
  args.GetReturnValue().Set(External::New(
      args.GetIsolate(),
      uWS::WebSocket<isServer>::prepareMessage(
          nativeString.getData(), nativeString.getLength(), opCode, false)));
}

template <bool isServer>
void sendPrepared(const FunctionCallbackInfo<Value> &args) {
  unwrapSocket<isServer>(args[0].As<External>())
      ->sendPrepared(
          (typename uWS::WebSocket<isServer>::PreparedMessage *)args[1]
              .As<External>()
              ->Value());
}

template <bool isServer>
void finalizeMessage(const FunctionCallbackInfo<Value> &args) {
  uWS::WebSocket<isServer>::finalizeMessage(
      (typename uWS::WebSocket<isServer>::PreparedMessage *)args[0]
          .As<External>()
          ->Value());
}

void forEach(const FunctionCallbackInfo<Value> &args) {
  Isolate *isolate = args.GetIsolate();
  uWS::Group<uWS::SERVER> *group =
      (uWS::Group<uWS::SERVER> *)args[0].As<External>()->Value();
  Local<Function> cb = Local<Function>::Cast(args[1]);
  Local<Context> context = isolate->GetCurrentContext();

  group->forEach([isolate, &cb, &context](uWS::WebSocket<uWS::SERVER> *webSocket) {
    Local<Value> argv[] = {getDataV8(webSocket, isolate)};
    cb->Call(context, Null(isolate), 1, argv);
  });
}

void getSize(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<uWS::SERVER> *group =
      (uWS::Group<uWS::SERVER> *)args[0].As<External>()->Value();
  GroupData *groupData = (GroupData *)group->getUserData();
  args.GetReturnValue().Set(Integer::New(args.GetIsolate(), groupData->size));
}

void startAutoPing(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<uWS::SERVER> *group =
      (uWS::Group<uWS::SERVER> *)args[0].As<External>()->Value();

  NativeString nativeString(args.GetIsolate(), args[2]);

  group->startAutoPing(
      args[1].As<Integer>()->Value(),
      nativeString.getData(), nativeString.getLength(), uWS::OpCode::BINARY);
}


void getSSLContext(const FunctionCallbackInfo<Value> &args) {
    Isolate* isolate = args.GetIsolate();
    if(args.Length() < 1 || !args[0]->IsObject()){
      MaybeLocal<String> msgSt = String::NewFromUtf8(isolate, "Error: One object expected", NewStringType::kNormal);
      isolate->ThrowException(Exception::TypeError(
      msgSt.ToLocalChecked()));
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

void listen(const FunctionCallbackInfo<Value> &args) {
  uWS::Group<uWS::SERVER> *group =
      (uWS::Group<uWS::SERVER> *)args[0].As<External>()->Value();
  hub.listen(args[1].As<Integer>()->Value(), nullptr, 0, group);
}

template <bool isServer>
struct Namespace {
  Local<Object> object;
  Namespace(Isolate *isolate) {
    object = Object::New(isolate);
    NODE_SET_METHOD(object, "send", send<isServer>);
    NODE_SET_METHOD(object, "close", closeSocket<isServer>);
    NODE_SET_METHOD(object, "terminate", terminateSocket<isServer>);
    NODE_SET_METHOD(object, "prepareMessage", prepareMessage<isServer>);
    NODE_SET_METHOD(object, "sendPrepared", sendPrepared<isServer>);
    NODE_SET_METHOD(object, "finalizeMessage", finalizeMessage<isServer>);

    Local<Object> group = Object::New(isolate);
    NODE_SET_METHOD(group, "onConnection", onConnection<isServer>);
    NODE_SET_METHOD(group, "onMessage", onMessage<isServer>);
    NODE_SET_METHOD(group, "onDisconnection", onDisconnection<isServer>);

    if (!isServer) {
      NODE_SET_METHOD(group, "onError", onError);
    } else {
      NODE_SET_METHOD(group, "forEach", forEach);
      NODE_SET_METHOD(group, "getSize", getSize);
      NODE_SET_METHOD(group, "startAutoPing", startAutoPing);
      NODE_SET_METHOD(group, "listen", listen);
    }

    NODE_SET_METHOD(group, "onPing", onPing<isServer>);
    NODE_SET_METHOD(group, "onPong", onPong<isServer>);
    NODE_SET_METHOD(group, "create", createGroup<isServer>);
    NODE_SET_METHOD(group, "delete", deleteGroup<isServer>);
    NODE_SET_METHOD(group, "close", closeGroup<isServer>);
    NODE_SET_METHOD(group, "terminate", terminateGroup<isServer>);
    NODE_SET_METHOD(group, "broadcast", broadcast<isServer>);

    MaybeLocal<String> groupSt = String::NewFromUtf8(isolate, "group", NewStringType::kNormal);
    object->Set(groupSt.ToLocalChecked(), group);
  }
};
