#include "../../uWebSockets/src/Hub.h"
#include "addon.h"

void Main(Local<Object> exports)
{
    Isolate *isolate = exports->GetIsolate();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "server", NewStringType::kNormal).ToLocalChecked(), Namespace(isolate).object);
    NODE_SET_METHOD(exports, "getSSLContext", getSSLContext);
    NODE_SET_METHOD(exports, "setUserData", setUserData);
    NODE_SET_METHOD(exports, "clearUserData", clearUserData);
    NODE_SET_METHOD(exports, "getAddress", getAddress);
    NODE_SET_METHOD(exports, "transfer", transfer);
    NODE_SET_METHOD(exports, "upgrade", upgrade);
    NODE_SET_METHOD(exports, "setNoop", setNoop);
    registerCheck(isolate);
}

NODE_MODULE(eiows, Main)
