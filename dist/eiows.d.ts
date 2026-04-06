export type WebSocketMessage = string | Buffer;

export interface SocketAddress {
    remotePort: number;
    remoteAddress: string;
    remoteFamily: string;
}

export class WebSocket {
    constructor(external: any, compressEnabled?: boolean, compressThreshold?: number);
    external: any;
    readonly CONNECTING: number;
    readonly OPEN: number;
    readonly CLOSING: number;
    readonly CLOSED: number;
    readyState: number;
    compressEnabled: boolean;
    compressThreshold: number;
    internalOnMessage: (message: WebSocketMessage, isBinary: boolean) => void;
    internalOnClose: (code: number, message: string) => void;
    on(eventName: string, listener: (...args: any[]) => void): this;
    once(eventName: string, listener: (...args: any[]) => void): this;
    removeListener(eventName: string, listener: (...args: any[]) => void): this;
    readonly _socket: SocketAddress;
    send(message: string | Buffer | ArrayBuffer | ArrayBufferView, options?: any, cb?: (err?: Error) => void): void;
    close(code?: number, data?: string | Buffer | ArrayBuffer | ArrayBufferView): void;
}

export class Server {
    constructor(options: any);
    serverGroup: any;
    _pendingUpgradeCallbacks: Array<(socket: WebSocket) => void>;
    _noDelay: any;
    _compressEnabled: boolean;
    _compressThreshold: number;
    handleUpgrade(request: any, socket: any, upgradeHead: any, callback: (socket: WebSocket) => void): any;
    close(): void;
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
