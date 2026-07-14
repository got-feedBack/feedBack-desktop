// PR #107 review: the native monitor-mute arbiter REFCOUNTS suppressions
// (SourceChain::setMonitorMuteSuppressed — true = acquire, false = release),
// but it kept the old boolean signature. The renderer's rebuild guard is
// deliberately unpaired: resolveChainRebuildGuard() leaves the suppression on
// when a rebuild produced an empty chain, and returns early without releasing
// while a provider route is still resolving. Against the old LATCHED BOOL that
// was self-correcting (repeated trues were idempotent, any false reset it);
// against a refcount every unpaired call is a permanent +1, so after a couple
// of song loads the count can never return to zero and monitor mute is silently
// dead for the rest of the session.
//
// aeSetMonitorMuteSuppressed() therefore holds AT MOST ONE native suppression.
// These cases pin that, plus the rollback: the latch mirrors the native
// refcount, so it may only stay flipped if the IPC actually landed.

'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const vm = require('node:vm');

const ROOT = path.join(__dirname, '..');
const SCREEN_JS = fs.readFileSync(path.join(ROOT, 'src', 'renderer', 'screen.js'), 'utf8');

function extractFunction(src, name) {
    const sig = `function ${name}(`;
    const start = src.indexOf(sig);
    assert.ok(start !== -1, `function '${name}' not found`);
    let i = src.indexOf('{', src.indexOf(')', start));
    let depth = 1;
    i++;
    while (i < src.length && depth > 0) {
        if (src[i] === '{') depth++;
        else if (src[i] === '}') depth--;
        i++;
    }
    assert.ok(depth === 0, `unbalanced braces in '${name}'`);
    return src.slice(start, i);
}

// Build a sandbox with the real function plus a fake native api that records
// every acquire/release and can be made to fail. `calls` is the ground truth
// for what the native refcount would have done.
function makeHarness({ mode = 'ok' } = {}) {
    const calls = [];
    const setMonitorMuteSuppressed = (suppressed) => {
        if (mode === 'throw') { calls.push({ suppressed, outcome: 'threw' }); throw new Error('sync boom'); }
        if (mode === 'reject') {
            calls.push({ suppressed, outcome: 'rejected' });
            return Promise.reject(new Error('ipc boom'));
        }
        calls.push({ suppressed, outcome: 'ok' });
        return Promise.resolve();
    };
    const audio = mode === 'downlevel' ? {} : { setMonitorMuteSuppressed };
    const ctx = {
        window: { feedBackDesktop: { audio } },
        // The native refcount, simulated: clamped at 0 exactly like SourceChain's
        // compare_exchange loop, so an unpaired release can't underflow.
        nativeCount: 0,
    };
    vm.createContext(ctx);
    vm.runInContext(
        'let aeMonitorMuteSuppressionHeld = false;\n'
        + extractFunction(SCREEN_JS, 'aeSetMonitorMuteSuppressed')
        + '\nglobalThis.__call = aeSetMonitorMuteSuppressed;'
        + '\nglobalThis.__held = () => aeMonitorMuteSuppressionHeld;',
        ctx,
    );
    return {
        calls,
        set: (v) => ctx.__call(v),
        held: () => ctx.__held(),
        // Replay the recorded calls through the native refcount semantics.
        nativeCount: () => calls.reduce((n, c) => {
            if (c.outcome !== 'ok') return n;               // never reached the engine
            return c.suppressed ? n + 1 : Math.max(0, n - 1);
        }, 0),
    };
}

const flush = () => new Promise((r) => setImmediate(r));

test('repeated unpaired acquires hold at most ONE native suppression', async () => {
    const h = makeHarness();
    // Three song loads whose guard never releases (empty-chain / provider-pending
    // branches). Under a raw refcount this would be +3 and never recoverable.
    h.set(true); h.set(true); h.set(true);
    await flush();
    assert.equal(h.calls.filter((c) => c.suppressed).length, 1, 'only one acquire may reach the engine');
    assert.equal(h.nativeCount(), 1);

    // ...and one release still returns the count to zero, so monitor mute works.
    h.set(false);
    await flush();
    assert.equal(h.nativeCount(), 0, 'a single release must fully un-suppress');
    assert.equal(h.held(), false);
});

test('acquire/release cycles stay balanced across many song loads', async () => {
    const h = makeHarness();
    for (let i = 0; i < 25; i++) {
        h.set(true);          // clearChainForNewSong + preload both call the guard
        h.set(true);
        await flush();
        h.set(false);         // resolveChainRebuildGuard
        await flush();
    }
    assert.equal(h.nativeCount(), 0, 'refcount must not drift across sessions');
    assert.equal(h.held(), false);
});

test('a rejected release rolls the latch back so the next release retries', async () => {
    // The bug this guards: if the latch flipped to "released" on an IPC that
    // never landed, every later release would short-circuit while the native
    // count stayed held — stuck suppression, one level up from the C++ leak.
    const calls = [];
    let failNext = false;
    const ctx = {
        window: {
            feedBackDesktop: {
                audio: {
                    setMonitorMuteSuppressed: (s) => {
                        if (failNext) { calls.push({ suppressed: s, outcome: 'rejected' }); return Promise.reject(new Error('boom')); }
                        calls.push({ suppressed: s, outcome: 'ok' });
                        return Promise.resolve();
                    },
                },
            },
        },
    };
    vm.createContext(ctx);
    vm.runInContext(
        'let aeMonitorMuteSuppressionHeld = false;\n'
        + extractFunction(SCREEN_JS, 'aeSetMonitorMuteSuppressed')
        + '\nglobalThis.__call = aeSetMonitorMuteSuppressed;'
        + '\nglobalThis.__held = () => aeMonitorMuteSuppressionHeld;',
        ctx,
    );

    ctx.__call(true);                       // acquire lands: native = 1
    await flush();
    assert.equal(ctx.__held(), true);

    failNext = true;
    ctx.__call(false);                      // release REJECTS: native still 1
    await flush();
    assert.equal(ctx.__held(), true, 'a failed release must not leave the latch "released"');

    failNext = false;
    ctx.__call(false);                      // retry must actually be attempted
    await flush();
    const releases = calls.filter((c) => !c.suppressed);
    assert.equal(releases.length, 2, 'the retry must reach the engine, not short-circuit');
    assert.equal(releases.at(-1).outcome, 'ok');
    assert.equal(ctx.__held(), false);
});

test('a downlevel addon without the arbiter is a clean no-op', async () => {
    const h = makeHarness({ mode: 'downlevel' });
    assert.doesNotThrow(() => { h.set(true); h.set(false); });
    await flush();
    assert.equal(h.calls.length, 0);
    assert.equal(h.held(), false, 'nothing was acquired, so nothing may be recorded as held');
});

test('a synchronously throwing bridge rolls the latch back', async () => {
    const h = makeHarness({ mode: 'throw' });
    assert.doesNotThrow(() => h.set(true));
    await flush();
    assert.equal(h.held(), false, 'a throw means nothing was acquired');
});
