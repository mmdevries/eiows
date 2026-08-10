'use strict';

const { copyFileSync, renameSync, rmSync } = require('node:fs');
const path = require('node:path');

const [source, destination] = process.argv.slice(2);
if (!source || !destination) throw new Error('source and destination are required');

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
