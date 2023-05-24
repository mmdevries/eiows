NODE=`(node --version)`

default:
	git clone -c advice.detachedHead=false --depth 1 https://github.com/nodejs/node -b $(NODE) nodejs/src/node
	mv nodejs/src/node/src/crypto/crypto_keys.h nodejs/src/node/src/crypto/crypto_keys.h.bak
	mv nodejs/src/node/src/crypto/crypto_tls.h nodejs/src/node/src/crypto/crypto_tls.h.bak
	mv nodejs/src/node/deps/openssl/config/archs/linux-x86_64/asm/include/openssl/configuration.h nodejs/src/node/deps/openssl/config/archs/linux-x86_64/asm/include/openssl/configuration.h.bak
	cat nodejs/src/node/src/crypto/crypto_keys.h.bak | awk '! /PROVIDER_KEYEXPORTREQUEST/ {print $0}' > nodejs/src/node/src/crypto/crypto_keys.h
	cat nodejs/src/node/src/crypto/crypto_tls.h.bak | awk '1;/is_awaiting_new_session/{print "  const SSLPointer *getSSL() const { return &ssl_; }"}' > nodejs/src/node/src/crypto/crypto_tls.h
	cat nodejs/src/node/deps/openssl/config/archs/linux-x86_64/asm/include/openssl/configuration.h.bak | awk '! /define OPENSSL_CONFIGURED_API/ {print $0}' > nodejs/src/node/deps/openssl/config/archs/linux-x86_64/asm/include/openssl/configuration.h

.PHONY: clean
clean:
	rm -rf nodejs/src/node
