#include "Extensions.h"

#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace eioWS {
namespace {

struct ExtensionParameter {
    std::string name;
    std::string value;
    bool hasValue = false;
};

struct ExtensionOffer {
    std::string name;
    std::vector<ExtensionParameter> parameters;
};

bool isTokenCharacter(unsigned char character) {
    if ((character >= '0' && character <= '9') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= 'a' && character <= 'z')) {
        return true;
    }
    switch (character) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

void lowerAscii(std::string &value) {
    for (char &character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
}

class ExtensionsParser {
    const std::string &input;
    size_t position = 0;

    void skipWhitespace() {
        while (position < input.size() &&
               (input[position] == ' ' || input[position] == '\t')) {
            position++;
        }
    }

    bool readToken(std::string &token) {
        const size_t start = position;
        while (position < input.size() &&
               isTokenCharacter(static_cast<unsigned char>(input[position]))) {
            position++;
        }
        if (position == start) {
            return false;
        }
        token.assign(input, start, position - start);
        lowerAscii(token);
        return true;
    }

    bool readQuotedString(std::string &value) {
        if (position == input.size() || input[position] != '"') {
            return false;
        }
        position++;
        while (position < input.size()) {
            const unsigned char character = static_cast<unsigned char>(input[position++]);
            if (character == '"') {
                return true;
            }
            if (character == '\\') {
                if (position == input.size()) {
                    return false;
                }
                const unsigned char escaped = static_cast<unsigned char>(input[position++]);
                if (escaped < 0x20 || escaped == 0x7f) {
                    return false;
                }
                value.push_back(static_cast<char>(escaped));
            } else {
                const bool valid = character == '\t' || character == ' ' ||
                    character == 0x21 ||
                    (character >= 0x23 && character <= 0x5b) ||
                    (character >= 0x5d && character != 0x7f);
                if (!valid) {
                    return false;
                }
                value.push_back(static_cast<char>(character));
            }
        }
        return false;
    }

    bool readParameter(ExtensionParameter &parameter) {
        skipWhitespace();
        if (!readToken(parameter.name)) {
            return false;
        }
        skipWhitespace();
        if (position == input.size() || input[position] != '=') {
            return true;
        }
        parameter.hasValue = true;
        position++;
        skipWhitespace();
        if (position < input.size() && input[position] == '"') {
            return readQuotedString(parameter.value);
        }
        return readToken(parameter.value);
    }

public:
    explicit ExtensionsParser(const std::string &input) : input(input) {}

    bool parse(std::vector<ExtensionOffer> &offers) {
        skipWhitespace();
        if (position == input.size()) {
            return true;
        }

        for (;;) {
            ExtensionOffer offer;
            if (!readToken(offer.name)) {
                return false;
            }

            for (;;) {
                skipWhitespace();
                if (position == input.size() || input[position] == ',') {
                    break;
                }
                if (input[position++] != ';') {
                    return false;
                }
                ExtensionParameter parameter;
                if (!readParameter(parameter)) {
                    return false;
                }
                offer.parameters.push_back(std::move(parameter));
            }
            offers.push_back(std::move(offer));

            if (position == input.size()) {
                return true;
            }
            position++;
            skipWhitespace();
            if (position == input.size()) {
                return false;
            }
        }
    }
};

bool isWindowBits(const ExtensionParameter &parameter, bool valueRequired) {
    if (!parameter.hasValue) {
        return !valueRequired;
    }
    if (parameter.value.size() == 1) {
        return parameter.value[0] >= '8' && parameter.value[0] <= '9';
    }
    return parameter.value.size() == 2 && parameter.value[0] == '1' &&
        parameter.value[1] >= '0' && parameter.value[1] <= '5';
}

bool isCompatibleOffer(const ExtensionOffer &offer, int wantedOptions) {
    std::unordered_set<std::string> seen;
    for (const ExtensionParameter &parameter : offer.parameters) {
        if (!seen.insert(parameter.name).second) {
            return false;
        }
        if (parameter.name == "server_no_context_takeover") {
            if (parameter.hasValue || !(wantedOptions & SERVER_NO_CONTEXT_TAKEOVER)) {
                return false;
            }
        } else if (parameter.name == "client_no_context_takeover") {
            if (parameter.hasValue) {
                return false;
            }
        } else if (parameter.name == "server_max_window_bits") {
            // This implementation always uses a 32 KiB server window. The
            // RFC requires declining a constrained offer we cannot honor.
            if (!isWindowBits(parameter, true)) {
                return false;
            }
            return false;
        } else if (parameter.name == "client_max_window_bits") {
            if (!isWindowBits(parameter, false)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

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

void ExtensionsNegotiator::readOffer(const std::string &offer) {
    if (!(options & PERMESSAGE_DEFLATE)) {
        options &= ~(CLIENT_NO_CONTEXT_TAKEOVER |
                     SERVER_NO_CONTEXT_TAKEOVER |
                     SLIDING_DEFLATE_WINDOW);
        return;
    }

    std::vector<ExtensionOffer> offers;
    ExtensionsParser parser(offer);
    bool accepted = false;
    if (parser.parse(offers)) {
        for (const ExtensionOffer &candidate : offers) {
            if (candidate.name == "permessage-deflate" &&
                isCompatibleOffer(candidate, options)) {
                accepted = true;
                break;
            }
        }
    }

    if (!accepted) {
        options &= ~PERMESSAGE_DEFLATE;
        options &= ~(CLIENT_NO_CONTEXT_TAKEOVER |
                     SERVER_NO_CONTEXT_TAKEOVER |
                     SLIDING_DEFLATE_WINDOW);
    }
}

int ExtensionsNegotiator::getNegotiatedOptions() const {
    return options;
}

} // namespace eioWS
