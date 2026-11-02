'use strict';

const { createHash, randomBytes } = require('node:crypto');
const { copyFileSync, createReadStream, createWriteStream, existsSync, lstatSync,
    mkdirSync, readFileSync, renameSync, rmSync, statSync, writeFileSync,
    mkdtempSync } = require('node:fs');
const { get } = require('node:https');
const { homedir, hostname, tmpdir } = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { pipeline } = require('node:stream/promises');
const { setTimeout: delay } = require('node:timers/promises');

const repositoryDirectory = path.join(__dirname, '..');
const nodeVersion = process.versions.node;
const nodeMajor = Number(nodeVersion.split('.')[0]);
const nodeModuleVersion = process.versions.modules;
const supportedMajors = new Set([22, 24, 26]);

if (!supportedMajors.has(nodeMajor)) {
    throw new Error(
        `eiows native transport supports Node.js 22, 24 and 26; found ${process.version}`
    );
}

function request(url, redirects = 0) {
    return new Promise((resolve, reject) => {
        const requestHandle = get(url, (response) => {
            if (response.statusCode >= 300 && response.statusCode < 400 && response.headers.location) {
                response.resume();
                if (redirects >= 5) {
                    reject(new Error(`too many redirects while downloading ${url}`));
                    return;
                }
                resolve(request(new URL(response.headers.location, url), redirects + 1));
                return;
            }
            if (response.statusCode !== 200) {
                response.resume();
                reject(new Error(`download failed with HTTP ${response.statusCode}: ${url}`));
                return;
            }
            resolve(response);
        });
        requestHandle.setTimeout(30000, () => {
            requestHandle.destroy(new Error(`download timed out: ${url}`));
        });
        requestHandle.once('error', reject);
    });
}

async function downloadText(url) {
    const response = await request(url);
    const chunks = [];
    for await (const chunk of response) chunks.push(chunk);
    return Buffer.concat(chunks).toString('utf8');
}

async function downloadFile(url, destination) {
    const temporary = `${destination}.${process.pid}.tmp`;
    rmSync(temporary, { force: true });
    try {
        await pipeline(await request(url), createWriteStream(temporary, { mode: 0o600 }));
        renameSync(temporary, destination);
    } finally {
        rmSync(temporary, { force: true });
    }
}

async function sha256(filename) {
    const hash = createHash('sha256');
    for await (const chunk of createReadStream(filename)) hash.update(chunk);
    return hash.digest('hex');
}

function sourceVersion(sourceDirectory) {
    const header = readFileSync(path.join(sourceDirectory, 'src', 'node_version.h'), 'utf8');
    const readPart = (name) => {
        const match = header.match(new RegExp(`^#define NODE_${name}_VERSION (\\d+)$`, 'm'));
        return match && match[1];
    };
    const major = readPart('MAJOR');
    const minor = readPart('MINOR');
    const patch = readPart('PATCH');
    return major && minor && patch ? `${major}.${minor}.${patch}` : null;
}

function validateSource(sourceDirectory) {
    const required = [
        'common.gypi',
        'src/async_wrap.h',
        'src/crypto/crypto_tls.h',
        'src/stream_base.h',
        'deps/ncrypto/ncrypto.h'
    ];
    return required.every((filename) => existsSync(path.join(sourceDirectory, filename))) &&
        sourceVersion(sourceDirectory) === nodeVersion;
}

function inspectCacheLock(lockDirectory) {
    try {
        const stat = lstatSync(lockDirectory);
        if (!stat.isDirectory()) return { owner: null };
        let owner;
        try {
            owner = JSON.parse(readFileSync(path.join(lockDirectory, 'owner.json'), 'utf8'));
        } catch (error) {
            if (error instanceof SyntaxError || error.code === 'ENOENT') return { owner: null };
            throw error;
        }
        if (!owner || !Number.isSafeInteger(owner.pid) || owner.pid <= 0 ||
            typeof owner.hostname !== 'string' ||
            typeof owner.token !== 'string' || !/^[a-f0-9]{32}$/.test(owner.token)) {
            return { owner: null };
        }
        return { owner };
    } catch (error) {
        if (error.code === 'ENOENT') return null;
        throw error;
    }
}

function cacheLockOwnerIsAlive(owner) {
    // A cache may be shared between hosts. A local PID cannot establish that a
    // process on another host has exited. Reused PIDs are also kept safely locked.
    if (owner.hostname !== hostname()) return true;
    try {
        process.kill(owner.pid, 0);
        return true;
    } catch (error) {
        if (error.code === 'ESRCH') return false;
        if (error.code === 'EPERM') return true;
        throw error;
    }
}

function recoverCacheLock(lockDirectory, owner) {
    const current = inspectCacheLock(lockDirectory);
    if (!current || !current.owner || current.owner.token !== owner.token ||
        cacheLockOwnerIsAlive(current.owner)) return false;
    // Keep the recovered directory as a nonempty tombstone. Every reclaimer of
    // this token uses the same destination, so a delayed competing reclaimer
    // cannot rename a newly acquired lock over it (rename refuses nonempty dirs).
    const recovered = `${lockDirectory}.recovered-${owner.token}`;
    try {
        renameSync(lockDirectory, recovered);
        return true;
    } catch (error) {
        if (['ENOENT', 'EEXIST', 'ENOTEMPTY', 'ENOTDIR', 'EISDIR'].includes(error.code)) {
            return false;
        }
        throw error;
    }
}

async function acquireCacheLock(lockDirectory, { timeoutMs = 120000, retryDelayMs } = {}) {
    const deadline = Date.now() + timeoutMs;
    const owner = {
        pid: process.pid,
        hostname: hostname(),
        token: randomBytes(16).toString('hex')
    };
    // Publish a fully populated directory atomically. There is no interval in
    // which a new lock exists without its owner metadata.
    const prepared = mkdtempSync(path.join(path.dirname(lockDirectory),
        `.${path.basename(lockDirectory)}-`));
    try {
        writeFileSync(path.join(prepared, 'owner.json'), JSON.stringify(owner), { mode: 0o600 });
        for (;;) {
            const current = inspectCacheLock(lockDirectory);
            if (!current) {
                try {
                    renameSync(prepared, lockDirectory);
                    let released = false;
                    return () => {
                        if (released) return;
                        released = true;
                        const locked = inspectCacheLock(lockDirectory);
                        if (locked && locked.owner && locked.owner.token === owner.token) {
                            // Remove the public path atomically before cleanup;
                            // recursive removal in place would briefly expose an
                            // empty directory that another rename could replace.
                            const retired = `${lockDirectory}.released-${owner.token}`;
                            renameSync(lockDirectory, retired);
                            rmSync(retired, { recursive: true, force: true });
                        }
                    };
                } catch (error) {
                    if (!['EEXIST', 'ENOTEMPTY', 'ENOTDIR', 'EISDIR'].includes(error.code)) {
                        throw error;
                    }
                }
            } else if (current.owner && recoverCacheLock(lockDirectory, current.owner)) {
                continue;
            }
            if (Date.now() >= deadline) {
                const detail = current && !current.owner
                    ? '; owner metadata is missing or invalid; check for a legacy installer before removing the lock'
                    : '';
                throw new Error(`timed out waiting for source cache lock ${lockDirectory}${detail}`);
            }
            const waitMs = retryDelayMs === undefined
                ? 100 + Math.floor(Math.random() * 100)
                : retryDelayMs;
            await delay(Math.min(waitMs, Math.max(1, deadline - Date.now())));
        }
    } finally {
        rmSync(prepared, { recursive: true, force: true });
    }
}

function build(sourceDirectory) {
    const source = path.join(repositoryDirectory, 'build', 'Release', 'eiows.node');
    const destination = path.join(
        repositoryDirectory,
        'dist',
        `eiows_${nodeModuleVersion}.node`
    );
    // A failed rebuild must not leave a stale binary loadable for this ABI.
    // Builds for other ABIs remain available for their matching runtimes.
    rmSync(destination, { force: true });
    // Version 10 used an unqualified filename. Never leave it available as a
    // fallback because this addon uses ABI-specific Node and V8 internals.
    rmSync(path.join(repositoryDirectory, 'dist', 'eiows.node'), { force: true });

    const nodeGyp = require.resolve('node-gyp/bin/node-gyp.js');
    const result = spawnSync(
        process.execPath,
        [nodeGyp, 'rebuild', `--nodedir=${sourceDirectory}`],
        { cwd: repositoryDirectory, env: process.env, stdio: 'inherit' }
    );
    if (result.error) throw result.error;
    if (result.status !== 0) {
        throw new Error(`node-gyp exited with status ${result.status}`);
    }

    if (!existsSync(source)) {
        throw new Error(`node-gyp did not produce ${source}`);
    }
    const temporary = path.join(
        path.dirname(destination),
        `.${path.basename(destination)}.${process.pid}.tmp`
    );
    rmSync(temporary, { force: true });
    try {
        copyFileSync(source, temporary);
        renameSync(temporary, destination);
    } finally {
        rmSync(temporary, { force: true });
    }
}

async function main() {
    const configuredSource = process.env.EIOWS_NODE_SOURCE_DIR || process.env.npm_config_nodedir;
    if (configuredSource) {
        const sourceDirectory = path.resolve(configuredSource);
        if (!validateSource(sourceDirectory)) {
            throw new Error(
                `${sourceDirectory} is not a complete Node.js ${nodeVersion} source tree`
            );
        }
        build(sourceDirectory);
        return;
    }

    const cacheRoot = process.env.EIOWS_NODE_SOURCE_CACHE ||
        path.join(process.env.npm_config_cache ||
            (process.platform === 'darwin'
                ? path.join(homedir(), 'Library', 'Caches')
                : path.join(homedir(), '.cache')) || tmpdir(), 'eiows-node-source');
    const sourceName = `node-v${nodeVersion}`;
    const sourceDirectory = path.join(cacheRoot, sourceName);
    const archiveName = `${sourceName}.tar.gz`;
    const archive = path.join(cacheRoot, archiveName);
    const marker = path.join(sourceDirectory, '.eiows-source.sha256');
    const baseUrl = `https://nodejs.org/dist/v${nodeVersion}`;

    mkdirSync(cacheRoot, { recursive: true });
    const releaseLock = await acquireCacheLock(path.join(cacheRoot, `${sourceName}.lock`));
    try {
        let expected = null;
        let cached = false;
        if (validateSource(sourceDirectory) && existsSync(marker)) {
            const storedChecksum = readFileSync(marker, 'utf8').trim();
            if (/^[a-f0-9]{64}$/.test(storedChecksum)) {
                expected = storedChecksum;
                cached = true;
            }
        }
        if (!expected) {
            const checksums = await downloadText(`${baseUrl}/SHASUMS256.txt`);
            const checksumLine = checksums.split('\n').find(
                (line) => line.endsWith(`  ${archiveName}`)
            );
            if (!checksumLine) throw new Error(`missing checksum for ${archiveName}`);
            expected = checksumLine.slice(0, 64);
        }
        if (!cached) {
            let actual = existsSync(archive) && statSync(archive).isFile()
                ? await sha256(archive)
                : null;
            if (actual !== expected) {
                await downloadFile(`${baseUrl}/${archiveName}`, archive);
                actual = await sha256(archive);
            }
            if (actual !== expected) {
                rmSync(archive, { force: true });
                throw new Error(
                    `SHA-256 mismatch for ${archiveName}: expected ${expected}, got ${actual}`
                );
            }

            const extractionRoot = mkdtempSync(path.join(cacheRoot, '.extract-'));
            try {
                const result = spawnSync('tar', ['-xzf', archive, '-C', extractionRoot], {
                    stdio: 'inherit'
                });
                const extractedSource = path.join(extractionRoot, sourceName);
                if (result.error) throw result.error;
                if (result.status !== 0 || !validateSource(extractedSource)) {
                    throw new Error(`failed to extract ${archiveName}`);
                }
                writeFileSync(path.join(extractedSource, '.eiows-source.sha256'), `${expected}\n`);
                rmSync(sourceDirectory, { recursive: true, force: true });
                renameSync(extractedSource, sourceDirectory);
            } finally {
                rmSync(extractionRoot, { recursive: true, force: true });
            }
        }
    } finally {
        releaseLock();
    }

    build(sourceDirectory);
}

module.exports = { acquireCacheLock, recoverCacheLock, sha256 };

if (require.main === module) {
    main().catch((error) => {
        console.error(error.stack || error);
        process.exitCode = 1;
    });
}
