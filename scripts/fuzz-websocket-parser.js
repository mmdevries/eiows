'use strict';

const native = require(`../dist/eiows_${process.versions.modules}.node`);

const PERMESSAGE_DEFLATE = 1;
const SERVER_NO_CONTEXT_TAKEOVER = 2;
const CLIENT_NO_CONTEXT_TAKEOVER = 4;
const DEFAULT_ITERATIONS = 25000;
const DEFAULT_SEED = 0x91e10da5;
const opCodes = [0, 1, 2, 3, 7, 8, 9, 10, 11, 15];

function readUnsignedInteger(value, fallback, name) {
    if (value === undefined) return fallback;
    const parsed = Number(value);
    if (!Number.isSafeInteger(parsed) || parsed <= 0 || parsed > 0xffffffff) {
        throw new RangeError(`${name} must be an integer between 1 and 4294967295`);
    }
    return parsed;
}

const iterations = readUnsignedInteger(
    process.env.FUZZ_ITERATIONS,
    DEFAULT_ITERATIONS,
    'FUZZ_ITERATIONS'
);
const seed = readUnsignedInteger(process.env.FUZZ_SEED, DEFAULT_SEED, 'FUZZ_SEED');
let randomState = seed >>> 0;

function random() {
    randomState ^= randomState << 13;
    randomState ^= randomState >>> 17;
    randomState ^= randomState << 5;
    return randomState >>> 0;
}

function randomBuffer(maxLength) {
    const buffer = Buffer.alloc(random() % (maxLength + 1));
    for (let index = 0; index < buffer.length; index++) buffer[index] = random();
    return buffer;
}

function clientFrame(payload, options) {
    const mask = Buffer.allocUnsafe(4);
    mask.writeUInt32BE(random());
    const headerLength = payload.length < 126 ? 2 : payload.length <= 0xffff ? 4 : 10;
    const frame = Buffer.allocUnsafe(headerLength + mask.length + payload.length);
    frame[0] = (options.fin ? 0x80 : 0) |
        (options.compressed ? 0x40 : 0) |
        options.opCode;
    if (payload.length < 126) {
        frame[1] = 0x80 | payload.length;
    } else if (payload.length <= 0xffff) {
        frame[1] = 0x80 | 126;
        frame.writeUInt16BE(payload.length, 2);
    } else {
        frame[1] = 0x80 | 127;
        frame.writeBigUInt64BE(BigInt(payload.length), 2);
    }
    mask.copy(frame, headerLength);
    for (let index = 0; index < payload.length; index++) {
        frame[headerLength + mask.length + index] = payload[index] ^ mask[index & 3];
    }
    return frame;
}

function fuzzInput() {
    if (random() & 1) return randomBuffer(512);

    const input = clientFrame(randomBuffer(512), {
        fin: Boolean(random() & 1),
        compressed: Boolean(random() & 1),
        opCode: opCodes[random() % opCodes.length]
    });
    const mutations = 1 + random() % 6;
    for (let mutation = 0; mutation < mutations; mutation++) {
        const index = random() % input.length;
        input[index] ^= 1 << (random() & 7);
    }
    return input;
}

for (let iteration = 0; iteration < iterations; iteration++) {
    const compression = iteration % 4 === 0;
    const options = compression
        ? PERMESSAGE_DEFLATE | SERVER_NO_CONTEXT_TAKEOVER | CLIENT_NO_CONTEXT_TAKEOVER
        : 0;
    const [session] = native.createSession(
        options,
        4096,
        compression
            ? 'permessage-deflate; server_no_context_takeover; client_no_context_takeover'
            : ''
    );

    try {
        if (random() % 16 === 0) {
            native.closeFrame(session, 1000, Buffer.from('fuzz'));
        }
        const messages = 1 + random() % 5;
        for (let message = 0; message < messages; message++) {
            const input = fuzzInput();
            let offset = 0;
            while (offset < input.length) {
                const chunkLength = Math.min(input.length - offset, 1 + random() % 31);
                native.consume(session, input.subarray(offset, offset + chunkLength));
                offset += chunkLength;
            }
            if (!input.length) native.consume(session, input);
        }
    } catch (error) {
        error.message = `fuzz seed ${seed}, iteration ${iteration}: ${error.message}`;
        throw error;
    } finally {
        native.dispose(session);
    }
}

process.stdout.write(`WebSocket parser fuzz passed (${iterations} iterations, seed ${seed})\n`);
