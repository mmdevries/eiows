'use strict';

const { readFileSync } = require('node:fs');

const filename = process.argv[2];
const expectedCases = Number(process.argv[3]);
const suite = process.argv[4];
if (!filename || !Number.isInteger(expectedCases) || expectedCases <= 0 || !suite) {
    throw new Error(
        'usage: node scripts/assert-autobahn-report.js ' +
        '<index.json> <expected-cases> <suite>'
    );
}

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

if (cases !== expectedCases) {
    throw new Error(`expected ${expectedCases} ${suite} cases, found ${cases}`);
}
if (failures.length) {
    throw new Error(`${suite} failures:\n${failures.join('\n')}`);
}
process.stdout.write(`${suite} conformance passed (${cases} cases)\n`);
