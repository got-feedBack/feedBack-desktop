const test = require('node:test');
const assert = require('node:assert/strict');
const { loadTs } = require('./_load-ts');

const { linuxUpdateDecision, isCommitSha } = loadTs('src/main/linux-update-decision.ts');

// Two distinct 40-char commit SHAs.
const N = 'a'.repeat(40);   // "current" nightly
const N1 = 'b'.repeat(40);  // a newer nightly
const OLD = 'c'.repeat(40); // some earlier staged sha

test('running build IS the latest nightly → idle', () => {
    assert.equal(linuxUpdateDecision(N, N, null), 'idle');
});

test('newer nightly available, nothing staged → download', () => {
    assert.equal(linuxUpdateDecision(N, N1, null), 'download');
});

test('newer nightly already staged this session → staged (no re-download)', () => {
    assert.equal(linuxUpdateDecision(N, N1, N1), 'staged');
});

test('unknown/dev build (no baked sha) always offers the update → download', () => {
    assert.equal(linuxUpdateDecision(null, N, null), 'download');
});

test('remote advanced past the sha we staged → download the newer one', () => {
    assert.equal(linuxUpdateDecision(N, N1, OLD), 'download');
});

test('no baked sha but this remote already staged → staged (not re-downloaded)', () => {
    assert.equal(linuxUpdateDecision(null, N, N), 'staged');
});

test('isCommitSha accepts a full 40-char hex SHA (either case)', () => {
    assert.equal(isCommitSha(N), true);
    assert.equal(isCommitSha('A'.repeat(40)), true);
});

test('isCommitSha rejects branch names and malformed values (fail-safe gate)', () => {
    // A branch-name target_commitish is exactly the case that would otherwise
    // loop-download forever; checkNowLinux() errors out instead.
    for (const bad of ['main', 'nightly', N.slice(0, 7), N + 'a', 'z'.repeat(40), '', null, undefined]) {
        assert.equal(isCommitSha(bad), false, `expected ${JSON.stringify(bad)} to be rejected`);
    }
});
