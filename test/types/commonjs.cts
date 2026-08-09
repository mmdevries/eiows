import eiows = require("eiows");

const server: eiows.Server = new eiows.Server({});
const engineIoServer: eiows.Server = new eiows.Server({
    perMessageDeflate: false,
    maxPayload: 30 * 1024
});
server.close();
engineIoServer.close();

new eiows.Server({ textAsString: true });

// @ts-expect-error textAsString must be a boolean.
new eiows.Server({ textAsString: "yes" });

// @ts-expect-error The native binding is intentionally not public.
void eiows.native;
