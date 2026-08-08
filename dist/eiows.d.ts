import { EventEmitter } from "node:events";
import { Duplex } from "node:stream";

export type WebSocketMessage = string | Buffer;
export interface SendOptions {
    compress?: boolean;
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
    readonly _socket: Duplex;
    send(message: string | Buffer | ArrayBuffer | ArrayBufferView, cb?: (err?: Error) => void): void;
    send(message: string | Buffer | ArrayBuffer | ArrayBufferView, options: SendOptions | null, cb?: (err?: Error) => void): void;
    close(code?: number, data?: string | Buffer | ArrayBuffer | ArrayBufferView): void;
    terminate(): void;
}

export class Server extends EventEmitter {
    constructor(options: any);
    serverGroup: any;
    _pendingUpgradeCallbacks: Array<(socket: WebSocket, request?: any) => void>;
    _noDelay: boolean;
    _compressEnabled: boolean;
    _compressThreshold: number;
    handleUpgrade(request: any, socket: Duplex, upgradeHead: Buffer, callback: (socket: WebSocket, request?: any) => void): void;
    close(callback?: () => void): void;
}

export const native: any;
export let compressThreshold: number;
export let PERMESSAGE_DEFLATE: number;
export let SERVER_NO_CONTEXT_TAKEOVER: number;
export let CLIENT_NO_CONTEXT_TAKEOVER: number;
export let SLIDING_DEFLATE_WINDOW: number;
export let CONNECTING: number;
export let OPCODE_TEXT: number;
export let OPCODE_BINARY: number;
export let OPCODE_PING: number;
export let OPEN: number;
export let CLOSING: number;
export let CLOSED: number;
