'use strict';

const { createHash } = require('node:crypto');
const { createWriteStream, existsSync, mkdirSync, readFileSync, renameSync,
    rmSync, statSync, writeFileSync, mkdtempSync } = require('node:fs');
const { get } = require('node:https');
const { homedir, tmpdir } = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { pipeline } = require('node:stream/promises');
const { setTimeout: delay } = require('node:timers/promises');

const repositoryDirectory = path.join(__dirname, '..');
const nodeVersion = process.versions.node;
const nodeMajor = Number(nodeVersion.split('.')[0]);
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

function sha256(filename) {
    return createHash('sha256').update(readFileSync(filename)).digest('hex');
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

async function acquireCacheLock(lockDirectory) {
    const deadline = Date.now() + 120000;
    for (;;) {
        try {
            mkdirSync(lockDirectory);
            return () => rmSync(lockDirectory, { recursive: true, force: true });
        } catch (error) {
            if (!error || error.code !== 'EEXIST') throw error;
            try {
                if (Date.now() - statSync(lockDirectory).mtimeMs > 10 * 60 * 1000) {
                    rmSync(lockDirectory, { recursive: true, force: true });
                    continue;
                }
            } catch (statError) {
                if (!statError || statError.code !== 'ENOENT') throw statError;
                continue;
            }
            if (Date.now() >= deadline) {
                throw new Error(`timed out waiting for source cache lock ${lockDirectory}`);
            }
            await delay(100 + Math.floor(Math.random() * 100));
        }
    }
}

function build(sourceDirectory) {
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
            if (!existsSync(archive) || !statSync(archive).isFile() ||
                sha256(archive) !== expected) {
                await downloadFile(`${baseUrl}/${archiveName}`, archive);
            }
            const actual = sha256(archive);
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

main().catch((error) => {
    console.error(error.stack || error);
    process.exitCode = 1;
});
