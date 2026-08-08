import eiows = require("eiows");

const server: eiows.Server = new eiows.Server({});
server.close();

// @ts-expect-error The native binding is intentionally not public.
void eiows.native;
