'use strict';

const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
const { createHash } = require('node:crypto');
const fs = require('node:fs');
const { hostname, tmpdir } = require('node:os');
const path = require('node:path');
const test = require('node:test');
const { acquireCacheLock, recoverCacheLock, sha256 } = require('../scripts/build');

function temporaryDirectory(t) {
    const directory = fs.mkdtempSync(path.join(tmpdir(), 'eiows-build-test-'));
    t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
    return directory;
}

function exitedPid() {
    const child = spawnSync(process.execPath, ['-e', 'console.log(process.pid)'], {
        encoding: 'utf8'
    });
    assert.equal(child.status, 0, child.stderr);
    return Number(child.stdout.trim());
}

function writeLock(directory, owner) {
    fs.mkdirSync(directory);
    fs.writeFileSync(path.join(directory, 'owner.json'), JSON.stringify(owner));
}

test('streams archive checksums and propagates read errors', async (t) => {
    const directory = temporaryDirectory(t);
    const filename = path.join(directory, 'archive');
    // Cross multiple stream chunks, including a partial final chunk.
    const content = Buffer.alloc(1024 * 1024 + 37);
    for (let index = 0; index < content.length; index++) content[index] = index % 251;
    fs.writeFileSync(filename, content);
    assert.equal(await sha256(filename), createHash('sha256').update(content).digest('hex'));
    await assert.rejects(sha256(path.join(directory, 'missing')), { code: 'ENOENT' });
});

test('keeps live cache owners regardless of lock age and releases idempotently', async (t) => {
    const directory = temporaryDirectory(t);
    const lock = path.join(directory, 'source.lock');
    const release = await acquireCacheLock(lock);
    const owner = fs.readFileSync(path.join(lock, 'owner.json'), 'utf8');
    fs.utimesSync(lock, new Date(0), new Date(0));
    await assert.rejects(acquireCacheLock(lock, { timeoutMs: 25, retryDelayMs: 2 }),
        /timed out waiting for source cache lock/);
    assert.equal(fs.readFileSync(path.join(lock, 'owner.json'), 'utf8'), owner);
    release();
    const releaseNext = await acquireCacheLock(lock);
    release();
    assert.equal(fs.existsSync(lock), true, 'old release must preserve the next owner');
    releaseNext();
    assert.deepEqual(fs.readdirSync(directory), []);
});

test('immediately recovers exited owners and preserves a replacement from stale recovery', async (t) => {
    const directory = temporaryDirectory(t);
    const lock = path.join(directory, 'source.lock');
    const owner = { pid: exitedPid(), hostname: hostname(), token: 'a'.repeat(32) };
    writeLock(lock, owner);
    const release = await acquireCacheLock(lock, { timeoutMs: 100, retryDelayMs: 2 });
    assert.equal(fs.existsSync(`${lock}.recovered-${owner.token}`), true);
    assert.equal(recoverCacheLock(lock, owner), false);
    assert.equal(JSON.parse(fs.readFileSync(path.join(lock, 'owner.json'))).pid, process.pid);
    release();
});

test('does not reclaim foreign-host or metadata-free locks', async (t) => {
    const directory = temporaryDirectory(t);
    const foreign = path.join(directory, 'foreign.lock');
    writeLock(foreign, {
        pid: exitedPid(), hostname: `${hostname()}-another-host`, token: 'b'.repeat(32)
    });
    await assert.rejects(acquireCacheLock(foreign, { timeoutMs: 20, retryDelayMs: 2 }),
        /timed out waiting/);
    assert.equal(fs.existsSync(path.join(foreign, 'owner.json')), true);
    const legacy = path.join(directory, 'legacy.lock');
    fs.mkdirSync(legacy);
    await assert.rejects(acquireCacheLock(legacy, { timeoutMs: 20, retryDelayMs: 2 }),
        /owner metadata is missing or invalid/);
    assert.equal(fs.existsSync(legacy), true);
});

test('serializes competing processes after recovering an interrupted installer', async (t) => {
    const directory = temporaryDirectory(t);
    const lock = path.join(directory, 'source.lock');
    const log = path.join(directory, 'critical-sections.log');
    const owner = { pid: exitedPid(), hostname: hostname(), token: 'c'.repeat(32) };
    writeLock(lock, owner);
    const modulePath = require.resolve('../scripts/build');
    const program = `
        const fs = require('node:fs');
        const { setTimeout: delay } = require('node:timers/promises');
        const { acquireCacheLock } = require(process.argv[1]);
        (async () => {
            const release = await acquireCacheLock(process.argv[2], {
                timeoutMs: 5000, retryDelayMs: 2
            });
            try {
                fs.appendFileSync(process.argv[3], 'start ' + process.pid + '\\n');
                await delay(15);
                fs.appendFileSync(process.argv[3], 'end ' + process.pid + '\\n');
            } finally { release(); }
        })().catch(error => { console.error(error); process.exitCode = 1; });
    `;
    await Promise.all(Array.from({ length: 8 }, () => new Promise((resolve, reject) => {
        const child = spawn(process.execPath, ['-e', program, modulePath, lock, log], {
            stdio: ['ignore', 'ignore', 'pipe'], timeout: 10000
        });
        let stderr = '';
        child.stderr.on('data', (chunk) => { stderr += chunk; });
        child.once('error', reject);
        child.once('exit', (code, signal) => code === 0
            ? resolve()
            : reject(new Error(`installer exited ${code}/${signal}: ${stderr}`)));
    })));
    const entries = fs.readFileSync(log, 'utf8').trim().split('\n');
    assert.equal(entries.length, 16);
    for (let index = 0; index < entries.length; index += 2) {
        assert.match(entries[index], /^start \d+$/);
        assert.equal(entries[index + 1], entries[index].replace('start ', 'end '));
    }
    assert.equal(fs.existsSync(lock), false);
});
