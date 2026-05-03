#ifndef EXTENSIONS_EIOWS_H
#define EXTENSIONS_EIOWS_H

#include <string>

namespace eioWS {
    enum Options : unsigned int {
        PERMESSAGE_DEFLATE = 1,
        SERVER_NO_CONTEXT_TAKEOVER = 2,
        CLIENT_NO_CONTEXT_TAKEOVER = 4,
        SLIDING_DEFLATE_WINDOW = 16
    };

    class ExtensionsNegotiator {
        protected:
            int options;
        public:
            ExtensionsNegotiator(int wantedOptions);
            std::string generateOffer() const;
            void readOffer(const std::string &offer);
            int getNegotiatedOptions() const;
    };
}

#endif // EXTENSIONS_EIOWS_H
