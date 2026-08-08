import eiows from './eiows.js';

export const {
    WebSocket,
    Server,
    compressThreshold,
    PERMESSAGE_DEFLATE,
    SERVER_NO_CONTEXT_TAKEOVER,
    CLIENT_NO_CONTEXT_TAKEOVER,
    SLIDING_DEFLATE_WINDOW,
    CONNECTING,
    OPCODE_TEXT,
    OPCODE_BINARY,
    OPCODE_PING,
    OPEN,
    CLOSING,
    CLOSED
} = eiows;

export default eiows;
