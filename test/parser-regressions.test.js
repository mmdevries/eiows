'use strict';

const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const test = require('node:test');
const zlib = require('node:zlib');

const nativePath = require.resolve(`../dist/eiows_${process.versions.modules}.node`);
const native = require(nativePath);
const NO_TAKEOVER = 7;
const addressSanitizer = Object.hasOwn(process.env, 'ASAN_OPTIONS') ||
    /(?:libasan|clang_rt\.asan)/i.test([
        process.env.LD_PRELOAD || '', process.env.DYLD_INSERT_LIBRARIES || ''
    ].join(' '));

function bytes(length) {
    const output = Buffer.alloc(length);
    let state = 0x91e10da5;
    for (let index = 0; index < length; index++) {
        state ^= state << 13;
        state ^= state >>> 17;
        state ^= state << 5;
        output[index] = state;
    }
    return output;
}

function compressed(payload, dictionary, final = false) {
    const output = zlib.deflateRawSync(payload, {
        dictionary,
        finishFlush: final ? zlib.constants.Z_FINISH : zlib.constants.Z_SYNC_FLUSH
    });
    // A final block must be followed by the stripped empty stored block's header.
    return final ? Buffer.concat([output, Buffer.from([0])]) : output.subarray(0, -4);
}

function clientFrame(payload, { compression = true, binary = true } = {}) {
    const headerLength = payload.length < 126 ? 2 : payload.length <= 0xffff ? 4 : 10;
    const output = Buffer.alloc(headerLength + 4 + payload.length);
    const mask = Buffer.from([0x12, 0x34, 0x56, 0x78]);
    output[0] = 0x80 | (compression ? 0x40 : 0) | (binary ? 2 : 1);
    if (headerLength === 2) output[1] = 0x80 | payload.length;
    else if (headerLength === 4) {
        output[1] = 0xfe;
        output.writeUInt16BE(payload.length, 2);
    } else {
        output[1] = 0xff;
        output.writeBigUInt64BE(BigInt(payload.length), 2);
    }
    mask.copy(output, headerLength);
    for (let index = 0; index < payload.length; index++) {
        output[headerLength + 4 + index] = payload[index] ^ mask[index & 3];
    }
    return output;
}

function withSession(options, maxPayload, operation) {
    const [session] = native.createSession(options, maxPayload, 'permessage-deflate');
    try { operation(session); }
    finally { native.dispose(session); }
}

test('compressed headers decode identically at every header split', () => {
    for (const length of [0, 5, 512, 70000]) {
        const payload = bytes(length);
        const encoded = compressed(payload);
        const headerLength = encoded.length < 126 ? 6 : encoded.length <= 0xffff ? 8 : 14;
        if (length === 512) assert.ok(encoded.length >= 126);
        if (length === 70000) assert.ok(encoded.length > 0xffff);
        for (let split = 1; split <= headerLength + 1; split++) {
            withSession(NO_TAKEOVER, 100000, (session) => {
                const frame = clientFrame(encoded);
                const events = [
                    ...native.consume(session, frame.subarray(0, split)),
                    ...native.consume(session, frame.subarray(split))
                ];
                assert.deepEqual(events, [[0, payload, true]], `length ${length}, split ${split}`);
            });
        }
    }
});

test('reconstructed headers preserve adjacent empty frames and borrowed payloads', () => {
    const payload = bytes(60000);
    const encoded = Buffer.concat([
        clientFrame(Buffer.alloc(0), { compression: false }),
        clientFrame(payload, { compression: false }),
        clientFrame(Buffer.from('last'), { compression: false })
    ]);
    for (let split = 1; split < 20; split++) {
        withSession(0, 100000, (session) => {
            const frame = Buffer.from(encoded);
            const events = [
                ...native.consume(session, frame.subarray(0, split)),
                ...native.consume(session, frame.subarray(split))
            ];
            native.consume(session, Buffer.alloc(0));
            assert.deepEqual(events, [[0, Buffer.alloc(0), true], [0, payload, true],
                [0, Buffer.from('last'), true]], `split ${split}`);
        });
    }
});

test('BFINAL blocks decode completely and retain history within and across messages', () => {
    const hello = Buffer.from('Hello');
    for (const options of [1, NO_TAKEOVER]) {
        withSession(options, 200000, (session) => {
            // RFC 7692 section 7.2.3.4's exact example, followed by a normal block.
            assert.deepEqual(native.consume(session, clientFrame(
                Buffer.from('f348cdc9c9070000', 'hex'), { binary: false })), [[0, 'Hello', false]]);
            assert.deepEqual(native.consume(session, clientFrame(
                compressed(hello), { binary: false })), [[0, 'Hello', false]]);
        });
        withSession(options, 200000, (session) => {
            const first = bytes(40000);
            const second = first.subarray(-24000);
            const firstBlock = compressed(first, undefined, true).subarray(0, -1);
            const secondBlock = compressed(second, first.subarray(-32768), true);
            assert.ok(secondBlock.length < 512, 'second block must reference the prior dictionary');
            const wire = Buffer.concat([firstBlock, secondBlock]);
            assert.deepEqual(native.consume(session, clientFrame(wire)),
                [[0, Buffer.concat([first, second]), true]]);
            if (options === 1) {
                const history = Buffer.concat([first, second]).subarray(-32768);
                const next = history.subarray(-20000);
                const nextBlock = compressed(next, history);
                assert.ok(nextBlock.length < 512, 'next message must reference the prior dictionary');
                assert.deepEqual(native.consume(session, clientFrame(nextBlock)),
                    [[0, next, true]]);
            }
        });
    }
});

test('payload limits and invalid blocks are enforced after a BFINAL boundary', () => {
    withSession(NO_TAKEOVER, 75, (session) => {
        const block = compressed(Buffer.alloc(50, 0x61), undefined, true);
        const wire = Buffer.concat([block.subarray(0, -1), block]);
        assert.ok(wire.length < 75);
        const events = native.consume(session, clientFrame(wire));
        assert.equal(events[0][0], 1);
        assert.equal(events[0][1].readUInt16BE(2), 1009);
        assert.deepEqual(events[1], [2, 1006, '']);
    });
    withSession(NO_TAKEOVER, 1000, (session) => {
        const events = native.consume(session, clientFrame(
            Buffer.from('f348cdc9c907000600', 'hex')));
        assert.equal(events[0][0], 1);
        assert.equal(events[0][1].readUInt16BE(2), 1007);
        assert.deepEqual(events[1], [2, 1006, '']);
    });
});

for (const workload of ['events', 'split-header']) {
    test(`parser releases burst allocations for live sessions: ${workload}`, {
        skip: addressSanitizer && 'ASan quarantine retains freed allocations; RSS budgets require an unsanitized allocator'
    }, () => {
        const script = `
            (async () => {
                const native = require(${JSON.stringify(nativePath)});
                const sessions = [];
                const events = ${JSON.stringify(workload)} === 'events';
                const frame = Buffer.alloc(events ? 65532 : 60008);
                if (events) {
                    for (let offset = 0; offset < frame.length; offset += 6) {
                        frame[offset] = 0x82;
                        frame[offset + 1] = 0x80;
                    }
                } else {
                    frame[0] = 0x82;
                    frame[1] = 0xfe;
                    frame.writeUInt16BE(60000, 2);
                }
                global.gc();
                const before = process.memoryUsage().rss;
                for (let index = 0; index < (events ? 256 : 2000); index++) {
                    const [session] = native.createSession(0, 100000, '');
                    sessions.push(session);
                    if (events) native.consume(session, frame);
                    else {
                        native.consume(session, frame.subarray(0, 1));
                        native.consume(session, frame.subarray(1));
                    }
                    if (index % 8 === 0) global.gc();
                }
                for (let index = 0; index < 3; index++) {
                    global.gc();
                    await new Promise(setImmediate);
                }
                console.log(JSON.stringify({ increase: process.memoryUsage().rss - before }));
                for (const session of sessions) native.dispose(session);
            })().catch((error) => { console.error(error); process.exitCode = 1; });
        `;
        const child = spawnSync(process.execPath, ['--expose-gc', '-e', script], {
            encoding: 'utf8', timeout: 60000
        });
        assert.equal(child.status, 0, child.stderr || child.error?.message);
        const { increase } = JSON.parse(child.stdout);
        assert.ok(increase < 64 * 1024 * 1024,
            `${workload} retained ${Math.round(increase / 1024 / 1024)} MiB`);
    });
}
