import eiows, { OPEN, Server, WebSocket } from "eiows";

const server: Server = new Server({});
const engineIoServer: Server = new Server({ perMessageDeflate: false });
let socket: WebSocket | undefined;
const open: number = OPEN;

socket = open === eiows.OPEN ? socket : undefined;
server.close(() => {
    socket = undefined;
});
engineIoServer.close();

// @ts-expect-error The native binding is intentionally not public.
void eiows.native;
