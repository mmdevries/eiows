#include "Extensions.h"

namespace eioWS {

    enum ExtensionTokens {
        TOK_PERMESSAGE_DEFLATE = 1838
    };

    class ExtensionsParser {
        public:
            bool perMessageDeflate = false;

            static int getToken(const char *&in, const char *stop);
            ExtensionsParser(const char *data, size_t length);
    };

    int ExtensionsParser::getToken(const char *&in, const char *stop) {
        while (in != stop && !isalnum(*in)) {
            in++;
        }

        int hashedToken = 0;
        while (in != stop && (isalnum(*in) || *in == '-' || *in == '_')) {
            if (isdigit(*in)) {
                hashedToken = hashedToken * 10 - (*in - '0');
            } else {
                hashedToken += *in;
            }
            in++;
        }
        return hashedToken;
    }

    ExtensionsParser::ExtensionsParser(const char *data, size_t length) {
        const char *stop = data + length;
        int token = 1;
        for (; token && token != TOK_PERMESSAGE_DEFLATE; token = getToken(data, stop));

        perMessageDeflate = (token == TOK_PERMESSAGE_DEFLATE);
    }

    ExtensionsNegotiator::ExtensionsNegotiator(int wantedOptions) {
        options = wantedOptions;
    }

    std::string ExtensionsNegotiator::generateOffer() const {
        std::string extensionsOffer;
        if (options & Options::PERMESSAGE_DEFLATE) {
            extensionsOffer += "permessage-deflate";

            if (options & Options::CLIENT_NO_CONTEXT_TAKEOVER) {
                extensionsOffer += "; client_no_context_takeover";
            }
            if (options & Options::SERVER_NO_CONTEXT_TAKEOVER) {
                extensionsOffer += "; server_no_context_takeover";
            }
        }
        return extensionsOffer;
    }

    void ExtensionsNegotiator::readOffer(std::string offer) {
        ExtensionsParser extensionsParser(offer.data(), offer.length());
        if (!((options & PERMESSAGE_DEFLATE) && extensionsParser.perMessageDeflate)) {
            options &= ~PERMESSAGE_DEFLATE;
            options &= ~(CLIENT_NO_CONTEXT_TAKEOVER | SERVER_NO_CONTEXT_TAKEOVER | SLIDING_DEFLATE_WINDOW);
        }
    }

    int ExtensionsNegotiator::getNegotiatedOptions() const {
        return options;
    }
}
