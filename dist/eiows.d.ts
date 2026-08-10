import { EventEmitter } from "node:events";
import { Duplex } from "node:stream";

export interface SocketInfo {
    readonly remoteAddress?: string;
    readonly remotePort?: number;
    readonly remoteFamily?: string;
}

export type WebSocketMessage = string | Buffer;
export interface SendOptions {
    compress?: boolean;
}

export interface PerMessageDeflateOptions {
    threshold?: number;
    clientNoContextTakeover?: boolean;
    serverNoContextTakeover?: boolean;
    [key: string]: unknown;
}

export interface ServerOptions {
    maxPayload?: number;
    noDelay?: boolean;
    perMessageDeflate?: boolean | PerMessageDeflateOptions;
    /**
     * Restore the eiows 10.0.1 behavior of emitting text as a JavaScript
     * string. By default text follows ws semantics: Buffer with isBinary false.
     */
    textAsString?: boolean;
    [key: string]: unknown;
}

export class WebSocket extends EventEmitter {
    external: any;
    readonly CONNECTING: number;
    readonly OPEN: number;
    readonly CLOSING: number;
    readonly CLOSED: number;
    readyState: number;
    compressEnabled: boolean;
    compressThreshold: number;
    readonly protocol: string;
    readonly extensions: string;
    binaryType: "nodebuffer";
    readonly bufferedAmount: number;
    readonly _sender: {
        sendFrame(parts: Array<string | ArrayBufferView>, callback?: (err?: Error) => void): void;
    };
    on(eventName: string, listener: (...args: any[]) => void): this;
    once(eventName: string, listener: (...args: any[]) => void): this;
    removeListener(eventName: string, listener: (...args: any[]) => void): this;
    /** Copied peer metadata; the Node Duplex is released after native takeover. */
    readonly _socket: SocketInfo;
    send(message: string | Buffer | ArrayBuffer | ArrayBufferView, cb?: (err?: Error) => void): void;
    send(message: string | Buffer | ArrayBuffer | ArrayBufferView, options: SendOptions | null, cb?: (err?: Error) => void): void;
    close(code?: number, data?: string | Buffer | ArrayBuffer | ArrayBufferView): void;
    terminate(): void;
}

export class Server extends EventEmitter {
    constructor(options: ServerOptions);
    serverGroup: any;
    _pendingUpgradeCallbacks: Array<(socket: WebSocket, request?: any) => void>;
    _noDelay: boolean;
    _compressEnabled: boolean;
    _compressThreshold: number;
    _textAsBuffer: boolean;
    handleUpgrade(request: any, socket: Duplex, upgradeHead: Buffer, callback: (socket: WebSocket, request?: any) => void): void;
    close(callback?: () => void): void;
}

export const compressThreshold: number;
export const PERMESSAGE_DEFLATE: number;
export const SERVER_NO_CONTEXT_TAKEOVER: number;
export const CLIENT_NO_CONTEXT_TAKEOVER: number;
export const SLIDING_DEFLATE_WINDOW: number;
export const CONNECTING: number;
export const OPCODE_TEXT: number;
export const OPCODE_BINARY: number;
export const OPCODE_PING: number;
export const OPEN: number;
export const CLOSING: number;
export const CLOSED: number;

declare const eiows: {
    WebSocket: typeof WebSocket;
    Server: typeof Server;
    compressThreshold: number;
    readonly PERMESSAGE_DEFLATE: number;
    readonly SERVER_NO_CONTEXT_TAKEOVER: number;
    readonly CLIENT_NO_CONTEXT_TAKEOVER: number;
    readonly SLIDING_DEFLATE_WINDOW: number;
    readonly CONNECTING: number;
    readonly OPCODE_TEXT: number;
    readonly OPCODE_BINARY: number;
    readonly OPCODE_PING: number;
    readonly OPEN: number;
    readonly CLOSING: number;
    readonly CLOSED: number;
};

export default eiows;
