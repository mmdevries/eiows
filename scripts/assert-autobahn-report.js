'use strict';

const { readFileSync } = require('node:fs');

const filename = process.argv[2];
if (!filename) throw new Error('usage: node scripts/assert-autobahn-report.js <index.json>');

const report = JSON.parse(readFileSync(filename, 'utf8'));
let cases = 0;
const failures = [];
for (const [agent, results] of Object.entries(report)) {
    for (const [id, result] of Object.entries(results)) {
        cases++;
        if (!['OK', 'NON-STRICT', 'INFORMATIONAL'].includes(result.behavior) ||
            !['OK', 'INFORMATIONAL'].includes(result.behaviorClose)) {
            failures.push(`${agent} case ${id}: ${result.behavior}/${result.behaviorClose}`);
        }
    }
}

if (cases !== 247) {
    throw new Error(`expected 247 Autobahn base cases, found ${cases}`);
}
if (failures.length) {
    throw new Error(`Autobahn failures:\n${failures.join('\n')}`);
}
process.stdout.write(`Autobahn base conformance passed (${cases} cases)\n`);
