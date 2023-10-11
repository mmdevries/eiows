export class Server {
    constructor(options: any);
    serverGroup: any;
    _upgradeCallback: typeof noop;
    _noDelay: any;
    handleUpgrade(request: any, socket: any, upgradeHead: any, callback: any): any;
    close(): void;
}
export const native: any;
declare function noop(): void;
export declare let PERMESSAGE_DEFLATE: number;
export declare let SLIDING_DEFLATE_WINDOW: number;
export declare let OPCODE_TEXT: number;
export declare let OPCODE_BINARY: number;
export declare let OPCODE_PING: number;
export declare let OPEN: number;
export declare let CLOSED: number;
export {};
