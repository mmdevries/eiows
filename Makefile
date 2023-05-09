CPP_OPTS := -DNODE_GYP_MODULE_NAME=eiows -DUSING_UV_SHARED=1 -DUSING_V8_SHARED=1 -DV8_DEPRECATION_WARNINGS=1 -DV8_DEPRECATION_WARNINGS -DV8_IMMINENT_DEPRECATION_WARNINGS -D_GLIBCXX_USE_CXX11_ABI=1 -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -D__STDC_FORMAT_MACROS -DOPENSSL_NO_PINSHARED -DOPENSSL_THREADS -DBUILDING_NODE_EXTENSION -shared -fPIC -pthread -Wall -Wextra -Wno-unused-parameter -m64 -O3 -std=gnu++17 -std=c++17 -Wno-unused-result -DOPENSSL_CONFIGURED_API=0x10100000L -DOPENSSL_API_COMPAT=0x10100000L -I uWebSockets/src uWebSockets/src/Extensions.cpp uWebSockets/src/Group.cpp uWebSockets/src/Networking.cpp uWebSockets/src/Hub.cpp uWebSockets/src/Node.cpp uWebSockets/src/WebSocket.cpp uWebSockets/src/Socket.cpp nodejs/src/addon.cpp

NODE=`(node --version)`

default:
	make nodesrc
	make module
	rm -rf nodejs/src/node
	chmod u+x dist/eiows_$(NODE).node
testbuild:
	make nodesrc
	make module
	chmod u+x dist/eiows_$(NODE).node
nodesrc:
	git clone --depth 1 https://github.com/nodejs/node -b $(NODE) nodejs/src/node
	sed -i '/PROVIDER_KEYEXPORTREQUEST/d' nodejs/src/node/src/crypto/crypto_keys.h
	sed -i '/is_awaiting_new_session.*/a const SSLPointer *getSSL() const { return &ssl_; }' nodejs/src/node/src/crypto/crypto_tls.h

module:
	g++ $(CPP_OPTS) -I nodejs/src/node/src -I nodejs/src/node/deps/uv/include -I nodejs/src/node/deps/v8/include -I nodejs/src/node/deps/openssl/openssl/include -I nodejs/src/node/deps/zlib -s -o dist/eiows_$(NODE).node

.PHONY: clean
clean:
	rm -f dist/eiows_*.node
	rm -rf nodejs/src/node
