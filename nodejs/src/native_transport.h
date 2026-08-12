#ifndef EIOWS_NATIVE_TRANSPORT_H
#define EIOWS_NATIVE_TRANSPORT_H

#include <node_api.h>

#include "../../uWebSockets/src/StreamWebSocket.h"

#include <cstddef>
#include <cstdint>

namespace eiowsNode {

class NativeTransport;

bool initializeNativeEnvironment(napi_env env);
NativeTransport *attachNativeTransport(napi_env env,
                                       eioWS::StreamWebSocket *session,
                                       napi_value handle,
                                       napi_value owner,
                                       bool textAsBuffer,
                                       bool encrypted,
                                       size_t maxBackpressure,
                                       NativeTransport **storage);
int activateNativeTransport(NativeTransport *transport);
void terminateNativeTransport(NativeTransport *transport);
void detachNativeTransportStorage(NativeTransport *transport);
void destroyNativeTransport(NativeTransport *transport);
bool feedNativeTransport(NativeTransport *transport,
                         napi_value source,
                         char *data,
                         size_t length);
int writeNativeMessage(NativeTransport *transport,
                       napi_value input,
                       eioWS::OpCode opCode,
                       bool compress,
                       napi_value callback);
int writeNativeFrameList(NativeTransport *transport,
                         napi_value list,
                         napi_value callback);
int writeNativeFrame(NativeTransport *transport,
                     napi_value frame,
                     napi_value callback);
int writeNativeClose(NativeTransport *transport,
                     uint16_t code,
                     const char *reason,
                     size_t reasonLength);
size_t nativeBufferedAmount(const NativeTransport *transport);

} // namespace eiowsNode

#endif // EIOWS_NATIVE_TRANSPORT_H
