NODE := $(shell node --version)
NODE_VERSION := $(shell node --version | sed 's/^v//')
USE_NCRYPTO := $(shell if [ $$(printf '%s\n' $(NODE_VERSION) 22.14.0 | sort -V | head -n1) = "22.14.0" ]; then echo 1; else echo 0; fi)

default:
	git clone -c advice.detachedHead=false --depth 1 https://github.com/nodejs/node -b $(NODE) nodejs/src/node
	mv nodejs/src/node/src/crypto/crypto_tls.h nodejs/src/node/src/crypto/crypto_tls.h.bak
	@if [ "$(USE_NCRYPTO)" -eq "1" ]; then \
		awk '/ncrypto::SSLPointer ssl_;/ && !done { print " public:"; print "  const ncrypto::SSLPointer *getSSL() const { return &ssl_; }"; print " private:"; done = 1 } { print } END { if (!done) exit 1 }' nodejs/src/node/src/crypto/crypto_tls.h.bak > nodejs/src/node/src/crypto/crypto_tls.h; \
	else \
		awk '/SSLPointer ssl_;/ && !done { print " public:"; print "  const SSLPointer *getSSL() const { return &ssl_; }"; print " private:"; done = 1 } { print } END { if (!done) exit 1 }' nodejs/src/node/src/crypto/crypto_tls.h.bak > nodejs/src/node/src/crypto/crypto_tls.h; \
	fi

.PHONY: clean
clean:
	rm -rf nodejs/src/node
