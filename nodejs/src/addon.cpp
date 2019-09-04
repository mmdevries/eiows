#include "../../uWebSockets/src/uWS.h"
#include "addon.h"

void Main(Local<Object> exports)
{
  Isolate *isolate = exports->GetIsolate();

  MaybeLocal<String> serverSt = String::NewFromUtf8(isolate, "server", NewStringType::kNormal);
  MaybeLocal<String> clientSt = String::NewFromUtf8(isolate, "client", NewStringType::kNormal);
  exports->Set(serverSt.ToLocalChecked(), Namespace<uWS::SERVER>(isolate).object);
  exports->Set(clientSt.ToLocalChecked(), Namespace<uWS::CLIENT>(isolate).object);

  NODE_SET_METHOD(exports, "getSSLContext", getSSLContext);
  NODE_SET_METHOD(exports, "setUserData", setUserData<uWS::SERVER>);
  NODE_SET_METHOD(exports, "getUserData", getUserData<uWS::SERVER>);
  NODE_SET_METHOD(exports, "clearUserData", clearUserData<uWS::SERVER>);
  NODE_SET_METHOD(exports, "getAddress", getAddress<uWS::SERVER>);

  NODE_SET_METHOD(exports, "transfer", transfer);
  NODE_SET_METHOD(exports, "upgrade", upgrade);
  NODE_SET_METHOD(exports, "connect", connect);
  NODE_SET_METHOD(exports, "setNoop", setNoop);
  registerCheck(isolate);
}

NODE_MODULE(uws, Main)
