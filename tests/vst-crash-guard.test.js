// Unit tests for the VST crash guard: crash-counting (a plugin is blocklisted
// only after CRASH_BLOCK_THRESHOLD consecutive crashes of the same op), streak
// reset on success, per-op isolation, and the blocklist management API used by
// Settings → Audio (get/remove/clear).
//
// The guard imports { app } from 'electron'; we intercept that require with a
// mock whose userData is a temp dir (same transpile-on-load pattern as the
// other suites — see _load-ts.js). Each loadGuard() call compiles a FRESH
// module instance, so module-level state (blocklist, counts) starts empty —
// exactly like a fresh app process.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const Module = require('node:module');
const { loadTs } = require('./_load-ts');

const PLUGIN = 'C:/Program Files/Common Files/VST3/Vendor/Bad Plugin.vst3';
const PLUGIN_KEY = process.platform === 'win32' ? PLUGIN.toLowerCase() : PLUGIN;

function loadGuard(userData) {
    const mockApp = { getPath: () => userData, once: () => {} };
    const origLoad = Module._load;
    Module._load = function (request, ...rest) {
        if (request === 'electron') return { app: mockApp };
        return origLoad.call(this, request, ...rest);
    };
    try {
        return loadTs('src/main/vst-crash-guard.ts');
    } finally {
        Module._load = origLoad;
    }
}

function tmpUserData(t) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'vst-crash-guard-test-'));
    t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
    return dir;
}

function writeSentinel(dir, plugin, op) {
    fs.writeFileSync(path.join(dir, 'vst-load-sentinel.json'),
        JSON.stringify({ plugin, op, at: Date.now() }));
}

function readCounts(dir) {
    return JSON.parse(fs.readFileSync(path.join(dir, 'vst-crash-counts.json'), 'utf8'));
}

test('threshold is exported and positive', () => {
    const guard = loadGuard(os.tmpdir());
    assert.equal(typeof guard.CRASH_BLOCK_THRESHOLD, 'number');
    assert.ok(guard.CRASH_BLOCK_THRESHOLD >= 2);
});

test('a single crash does not blocklist the plugin', (t) => {
    const dir = tmpUserData(t);
    writeSentinel(dir, PLUGIN, 'load');
    const guard = loadGuard(dir);
    assert.deepEqual(guard.initVstCrashGuard(), []);
    assert.deepEqual(guard.getCrashBlocklist(), []);
    assert.deepEqual(guard.consumeStartupBlocked(), []);
    // The crash streak is persisted for the next startup.
    assert.equal(readCounts(dir)[PLUGIN_KEY].load, 1);
});

test('a plugin is blocklisted only after CRASH_BLOCK_THRESHOLD consecutive crashes', (t) => {
    const dir = tmpUserData(t);
    const guard0 = loadGuard(dir);
    const threshold = guard0.CRASH_BLOCK_THRESHOLD;

    // Session 1 is a clean start; each following session's init records the
    // crash that ended the previous one. So threshold crashes need
    // threshold+1 sessions.
    let guard = guard0;
    let blocked = [];
    let pending = [];
    for (let session = 1; session <= threshold + 1; session++) {
        guard = loadGuard(dir);
        blocked = guard.initVstCrashGuard();
        pending = guard.consumeStartupBlocked();
        if (session <= threshold) {
            assert.deepEqual(blocked, [], `crash ${session - 1}/${threshold} must not blocklist yet`);
            assert.deepEqual(pending, []);
        }
        // Simulate the crash: the app dies with the sentinel still on disk.
        writeSentinel(dir, PLUGIN, 'load');
    }

    // The threshold-th crash (recorded at the start of the last session)
    // promoted the plugin into the blocklist.
    assert.deepEqual(blocked, [PLUGIN_KEY]);
    assert.deepEqual(guard.getCrashBlocklist(), [PLUGIN]); // original-case display path
    assert.deepEqual(pending, [{ path: PLUGIN, count: threshold, op: 'load' }]);
    // consumeStartupBlocked drains.
    assert.deepEqual(guard.consumeStartupBlocked(), []);
    // The blocklist is persisted.
    const persisted = JSON.parse(fs.readFileSync(path.join(dir, 'vst-crash-blocklist.json'), 'utf8'));
    assert.deepEqual(persisted, [PLUGIN]);
});

test("a successful op resets that op's streak (intermittent crasher never blocklists)", (t) => {
    const dir = tmpUserData(t);
    const guard0 = loadGuard(dir);
    const threshold = guard0.CRASH_BLOCK_THRESHOLD;

    for (let i = 1; i <= threshold + 2; i++) {
        const guard = loadGuard(dir);
        const blocked = guard.initVstCrashGuard();
        assert.deepEqual(blocked, [], 'an intermittent crasher must never accumulate');
        // The load succeeds this session (breaks the load streak)...
        guard.armSentinel(PLUGIN, 'load');
        guard.disarmSentinel();
        // ...then the app dies during a load at the end of the session.
        writeSentinel(dir, PLUGIN, 'load');
    }
    // The streak oscillates 0 → 1 → 0 → 1 … and never reaches the threshold.
    assert.ok(readCounts(dir)[PLUGIN_KEY].load <= 1);
});

test('editor crashes accumulate even when loads succeed every session', (t) => {
    const dir = tmpUserData(t);
    const guard0 = loadGuard(dir);
    const threshold = guard0.CRASH_BLOCK_THRESHOLD;

    let guard = guard0;
    for (let session = 1; session <= threshold + 1; session++) {
        guard = loadGuard(dir);
        guard.initVstCrashGuard();
        // Load succeeds every session — must NOT reset the editor streak.
        guard.armSentinel(PLUGIN, 'load');
        guard.disarmSentinel();
        // Editor-open crashes at the end of every session.
        writeSentinel(dir, PLUGIN, 'editor');
    }
    // threshold editor crashes recorded → blocklisted; the load streak stayed 0.
    assert.deepEqual(guard.getCrashBlocklist(), [PLUGIN]);
    assert.equal(readCounts(dir)[PLUGIN_KEY].load, 0);
});

test('a successful editor-open resets the editor streak', (t) => {
    const dir = tmpUserData(t);
    writeSentinel(dir, PLUGIN, 'editor');
    let guard = loadGuard(dir);
    guard.initVstCrashGuard();
    assert.equal(readCounts(dir)[PLUGIN_KEY].editor, 1);

    // Next session: the editor-open survives (arm + disarm = success).
    guard = loadGuard(dir);
    guard.initVstCrashGuard();
    guard.armSentinel(PLUGIN, 'editor');
    guard.disarmSentinel();
    assert.equal(readCounts(dir)[PLUGIN_KEY].editor, 0);
});

test('removeFromCrashBlocklist removes one entry and its streaks', (t) => {
    const dir = tmpUserData(t);
    const guard0 = loadGuard(dir);
    const threshold = guard0.CRASH_BLOCK_THRESHOLD;
    let guard = guard0;
    for (let session = 1; session <= threshold + 1; session++) {
        guard = loadGuard(dir);
        guard.initVstCrashGuard();
        writeSentinel(dir, PLUGIN, 'load');
    }
    assert.deepEqual(guard.getCrashBlocklist(), [PLUGIN]);

    assert.equal(guard.removeFromCrashBlocklist(PLUGIN), true);
    assert.deepEqual(guard.getCrashBlocklist(), []);
    // Streaks are dropped too — a re-block needs a fresh run of crashes.
    assert.deepEqual(readCounts(dir), {});
    // Removing again is a no-op.
    assert.equal(guard.removeFromCrashBlocklist(PLUGIN), false);
    // The persisted file no longer lists the plugin.
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(dir, 'vst-crash-blocklist.json'), 'utf8')), []);
});

test('removeFromCrashBlocklist matches case-insensitively on Windows', (t) => {
    const dir = tmpUserData(t);
    fs.writeFileSync(path.join(dir, 'vst-crash-blocklist.json'), JSON.stringify([PLUGIN]));
    const guard = loadGuard(dir);
    guard.initVstCrashGuard();
    assert.equal(guard.removeFromCrashBlocklist(PLUGIN.toUpperCase()),
        process.platform === 'win32');
});

test('clearCrashBlocklist empties the list and the streaks', (t) => {
    const dir = tmpUserData(t);
    const other = 'C:/Program Files/Common Files/VST3/Vendor/Other Plugin.vst3';
    fs.writeFileSync(path.join(dir, 'vst-crash-blocklist.json'), JSON.stringify([PLUGIN, other]));
    writeSentinel(dir, PLUGIN, 'load');
    const guard = loadGuard(dir);
    guard.initVstCrashGuard();
    assert.equal(guard.getCrashBlocklist().length, 2);

    assert.equal(guard.clearCrashBlocklist(), 2);
    assert.deepEqual(guard.getCrashBlocklist(), []);
    assert.deepEqual(readCounts(dir), {});
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(dir, 'vst-crash-blocklist.json'), 'utf8')), []);
    // Clearing an empty list is a no-op returning 0.
    assert.equal(guard.clearCrashBlocklist(), 0);
});

test('a crash for an already-blocklisted plugin records no notification', (t) => {
    const dir = tmpUserData(t);
    fs.writeFileSync(path.join(dir, 'vst-crash-blocklist.json'), JSON.stringify([PLUGIN]));
    writeSentinel(dir, PLUGIN, 'load');
    const guard = loadGuard(dir);
    assert.deepEqual(guard.initVstCrashGuard(), [PLUGIN_KEY]);
    assert.deepEqual(guard.consumeStartupBlocked(), []);
});

test('legacy lowercase blocklist entries load and display', (t) => {
    const dir = tmpUserData(t);
    fs.writeFileSync(path.join(dir, 'vst-crash-blocklist.json'),
        JSON.stringify([PLUGIN.toLowerCase()]));
    const guard = loadGuard(dir);
    assert.deepEqual(guard.initVstCrashGuard(), [PLUGIN_KEY]);
    assert.deepEqual(guard.getCrashBlocklist(), [PLUGIN.toLowerCase()]);
});
