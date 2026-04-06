#ifndef WEBSOCKETPROTOCOL_EIOWS_H
#define WEBSOCKETPROTOCOL_EIOWS_H

#include <uv.h>
#include <cstring>
#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htobe64(x) OSSwapHostToBigInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#endif

namespace eioWS {
    enum OpCode : unsigned char {
        NONE = 0,
        TEXT = 1,
        BINARY = 2,
        CLOSE = 8,
        PING = 9,
        PONG = 10
    };

    // 24 bytes perfectly
    struct WebSocketState {
        public:
            static const unsigned int SHORT_MESSAGE_HEADER = 6;
            static const unsigned int MEDIUM_MESSAGE_HEADER = 8;
            static const unsigned int LONG_MESSAGE_HEADER = 14;

            // 16 bytes
            struct State {
                unsigned int wantsHead : 1;
                unsigned int spillLength : 4;
                int opStack : 2; // -1, 0, 1
                unsigned int lastFin : 1;

                // 15 bytes
                unsigned char spill[LONG_MESSAGE_HEADER - 1] = { 0 };
                OpCode opCode[2] = { NONE };

                State() {
                    wantsHead = true;
                    spillLength = 0;
                    opStack = -1;
                    lastFin = true;
                }

            } state;

            // 8 bytes
            unsigned int remainingBytes = 0;
            char mask[4];
    };

    struct WebSocketProtocolHooks {
        bool (*refusePayloadLength)(uint64_t length, WebSocketState *webSocketState);
        bool (*setCompressed)(WebSocketState *webSocketState);
        void (*forceClose)(WebSocketState *webSocketState);
        bool (*handleFragment)(char *data, size_t length, unsigned int remainingBytes, int opCode, bool fin, WebSocketState *webSocketState);
    };

    class WebSocketProtocol {
        static inline bool isFin(char *frame) {return *((unsigned char *) frame) & 128;}
        static inline bool isMasked(char *frame) {return ((unsigned char *) frame)[1] & 128;}
        static inline unsigned char getOpCode(char *frame) {return *((unsigned char *) frame) & 15;}
        static inline unsigned char payloadLength(char *frame) {return ((unsigned char *) frame)[1] & 127;}
        static inline bool rsv23(char *frame) {return *((unsigned char *) frame) & 48;}
        static inline bool rsv1(char *frame) {return *((unsigned char *) frame) & 64;}

        static inline void unmaskImprecise(char *dst, char *src, char *mask, unsigned int length) {
            for (unsigned int n = (length >> 2) + 1; n; n--) {
                *(dst++) = *(src++) ^ mask[0];
                *(dst++) = *(src++) ^ mask[1];
                *(dst++) = *(src++) ^ mask[2];
                *(dst++) = *(src++) ^ mask[3];
            }
        }

        static inline void unmaskImpreciseCopyMask(char *dst, char *src, char *maskPtr, unsigned int length) {
            char mask[4] = {maskPtr[0], maskPtr[1], maskPtr[2], maskPtr[3]};
            unmaskImprecise(dst, src, mask, length);
        }

        static inline void rotateMask(unsigned int offset, char *mask) {
            char originalMask[4] = {mask[0], mask[1], mask[2], mask[3]};
            mask[(0 + offset) & 3] = originalMask[0];
            mask[(1 + offset) & 3] = originalMask[1];
            mask[(2 + offset) & 3] = originalMask[2];
            mask[(3 + offset) & 3] = originalMask[3];
        }

        static inline void unmaskInplace(char *data, char *stop, char *mask) {
            while (data < stop) {
                *(data++) ^= mask[0];
                *(data++) ^= mask[1];
                *(data++) ^= mask[2];
                *(data++) ^= mask[3];
            }
        }

        static inline bool consumeMessage(uint64_t payLength, unsigned int messageHeader, char *&src, unsigned int &length, WebSocketState *wState, const WebSocketProtocolHooks &hooks) {
            if (getOpCode(src)) {
                if (wState->state.opStack == 1 || (!wState->state.lastFin && getOpCode(src) < 2)) {
                    hooks.forceClose(wState);
                    return true;
                }
                wState->state.opCode[++wState->state.opStack] = (OpCode) getOpCode(src);
            } else if (wState->state.opStack == -1) {
                hooks.forceClose(wState);
                return true;
            }
            wState->state.lastFin = isFin(src);

            if (hooks.refusePayloadLength(payLength, wState)) {
                hooks.forceClose(wState);
                return true;
            }

            if (payLength + messageHeader <= length) {
                unmaskImpreciseCopyMask(src + messageHeader - 4, src + messageHeader, src + messageHeader - 4, (unsigned int) payLength);
                if (hooks.handleFragment(src + messageHeader - 4, payLength, 0, wState->state.opCode[wState->state.opStack], isFin(src), wState)) {
                    return true;
                }

                if (isFin(src)) {
                    wState->state.opStack--;
                }

                src += payLength + messageHeader;
                length -= payLength + messageHeader;
                wState->state.spillLength = 0;
                return false;
            }

            wState->state.spillLength = 0;
            wState->state.wantsHead = false;
            wState->remainingBytes = (unsigned int) (payLength - length + messageHeader);
            bool fin = isFin(src);
            memcpy(wState->mask, src + messageHeader - 4, 4);
            unmaskImprecise(src, src + messageHeader, wState->mask, length - messageHeader);
            rotateMask(4 - ((length - messageHeader) & 3), wState->mask);
            hooks.handleFragment(src, length - messageHeader, wState->remainingBytes, wState->state.opCode[wState->state.opStack], fin, wState);
            return true;
        }

        static inline bool consumeContinuation(char *&src, unsigned int &length, WebSocketState *wState, const WebSocketProtocolHooks &hooks) {
            if (wState->remainingBytes <= length) {
                int n = wState->remainingBytes >> 2;
                unmaskInplace(src, src + n * 4, wState->mask);
                for (int i = 0, s = wState->remainingBytes & 3; i < s; i++) {
                    src[n * 4 + i] ^= wState->mask[i];
                }

                if (hooks.handleFragment(src, wState->remainingBytes, 0, wState->state.opCode[wState->state.opStack], wState->state.lastFin, wState)) {
                    return false;
                }

                if (wState->state.lastFin) {
                    wState->state.opStack--;
                }

                src += wState->remainingBytes;
                length -= wState->remainingBytes;
                wState->state.wantsHead = true;
                return true;
            }

            unmaskInplace(src, src + ((length >> 2) + 1) * 4, wState->mask);

            wState->remainingBytes -= length;
            if (hooks.handleFragment(src, length, wState->remainingBytes, wState->state.opCode[wState->state.opStack], wState->state.lastFin, wState)) {
                return false;
            }

            if (length & 3) {
                rotateMask(4 - (length & 3), wState->mask);
            }
            return false;
        }

        enum {
            SND_COMPRESSED = 64
        };

        public:
            static const unsigned int SHORT_MESSAGE_HEADER = 6;
            static const unsigned int MEDIUM_MESSAGE_HEADER = 8;
            static const unsigned int LONG_MESSAGE_HEADER = 14;

            // Based on utf8_check.c by Markus Kuhn, 2005
            // https://www.cl.cam.ac.uk/~mgk25/ucs/utf8_check.c
            // Optimized for predominantly 7-bit content by Alex Hultman, 2016
            // Licensed as Zlib, like the rest of this project
            static bool isValidUtf8(unsigned char *s, size_t length) {
                for (unsigned char *e = s + length; s != e; ) {
                    if (s + 4 <= e && ((*reinterpret_cast<uint32_t *>(s)) & 0x80808080) == 0) {
                        s += 4;
                    } else {
                        while (!(*s & 0x80)) {
                            if (++s == e) {
                                return true;
                            }
                        }

                        if ((s[0] & 0x60) == 0x40) {
                            if (s + 1 >= e || (s[1] & 0xc0) != 0x80 || (s[0] & 0xfe) == 0xc0) {
                                return false;
                            }
                            s += 2;
                        } else if ((s[0] & 0xf0) == 0xe0) {
                            if (s + 2 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 ||
                                    (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) || (s[0] == 0xed && (s[1] & 0xe0) == 0xa0)) {
                                return false;
                            }
                            s += 3;
                        } else if ((s[0] & 0xf8) == 0xf0) {
                            if (s + 3 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80 ||
                                    (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) || (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4) {
                                return false;
                            }
                            s += 4;
                        } else {
                            return false;
                        }
                    }
                }
                return true;
            }

            struct CloseFrame {
                uint16_t code;
                char const *message;
                size_t length;
            };

            static inline CloseFrame parseClosePayload(char *src, size_t length) {
                CloseFrame cf = {1005, "", 0};

                if (length == 1) {
                    return {1006, "", 0};
                }

                if (length >= 2) {
                    memcpy(&cf.code, src, 2);
                    cf = {ntohs(cf.code), src + 2, length - 2};
                    if (cf.code < 1000 || cf.code > 4999 || (cf.code > 1011 && cf.code < 4000) ||
                        (cf.code >= 1004 && cf.code <= 1006) || !isValidUtf8((unsigned char *) cf.message, cf.length)) {
                        return {1006, "", 0};
                    }
                }
                return cf;
            }

            static inline size_t formatClosePayload(char *dst, uint16_t code, const char *message, size_t length) {
                if (code && code != 1005 && code != 1006) {
                    code = htons(code);
                    memcpy(dst, &code, 2);
                    if (message && length && isValidUtf8((unsigned char *) message, length)) {
                        memcpy(dst + 2, message, length);
                        return length + 2;
                    }
                    return 2;
                }
                return 0;
            }

            static inline size_t formatMessage(char *dst, const char *src, size_t length, OpCode opCode, size_t reportedLength, bool compressed) {
                size_t messageLength;
                size_t headerLength;
                if (reportedLength < 126) {
                    headerLength = 2;
                    dst[1] = reportedLength;
                } else if (reportedLength <= UINT16_MAX) {
                    headerLength = 4;
                    dst[1] = 126;
                    *(reinterpret_cast<uint16_t *>(&dst[2])) = htons(reportedLength);
                } else {
                    headerLength = 10;
                    dst[1] = 127;
                    *(reinterpret_cast<uint64_t *>(&dst[2])) = htobe64(reportedLength);
                }

                dst[0] = 128 | (compressed ? SND_COMPRESSED : 0) | opCode;

                messageLength = headerLength + length;
                memcpy(dst + headerLength, src, length);

                return messageLength;
            }

            static inline void consume(char *src, unsigned int length, WebSocketState *wState, const WebSocketProtocolHooks &hooks) {
                if (wState->state.spillLength) {
                    src -= wState->state.spillLength;
                    length += wState->state.spillLength;
                    memcpy(src, wState->state.spill, wState->state.spillLength);
                }
                if (wState->state.wantsHead) {
    parseNext:
                    while (length >= SHORT_MESSAGE_HEADER) {
                        unsigned char opCode = getOpCode(src);
                        bool invalidCompressedFrame = rsv1(src) && (opCode == 0 || opCode > 2 || !hooks.setCompressed(wState));

                        if (!isMasked(src) || invalidCompressedFrame || rsv23(src) || (opCode > 2 && opCode < 8) ||
                                opCode > 10 || (opCode > 2 && (!isFin(src) || payloadLength(src) > 125))) {
                            hooks.forceClose(wState);
                            return;
                        }

                        if (payloadLength(src) < 126) {
                            if (consumeMessage(payloadLength(src), SHORT_MESSAGE_HEADER, src, length, wState, hooks)) {
                                return;
                            }
                        } else if (payloadLength(src) == 126) {
                            if (length < MEDIUM_MESSAGE_HEADER) {
                                break;
                            } else if (consumeMessage(ntohs(*reinterpret_cast<uint16_t *>(&src[2])), MEDIUM_MESSAGE_HEADER, src, length, wState, hooks)) {
                                return;
                            }
                        } else if (length < LONG_MESSAGE_HEADER) {
                            break;
                        } else if (consumeMessage(be64toh(*reinterpret_cast<uint64_t *>(&src[2])), LONG_MESSAGE_HEADER, src, length, wState, hooks)) {
                            return;
                        }
                    }
                    if (length) {
                        memcpy(wState->state.spill, src, length);
                        wState->state.spillLength = length;
                    }
                } else if (consumeContinuation(src, length, wState, hooks)) {
                    goto parseNext;
                }
            }

            static const int CONSUME_POST_PADDING = 4;
            static const int CONSUME_PRE_PADDING = LONG_MESSAGE_HEADER - 1;
    };
}

#endif // WEBSOCKETPROTOCOL_EIOWS_H
