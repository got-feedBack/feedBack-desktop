// Guards against the alpha-tester chain-duplication bug: the audio_engine
// panel must (1) never persist a Rig-Builder-owned live chain into
// localStorage, (2) drop Rig Builder plumbing stages from legacy saves on
// restore, and (3) never auto-load (default preset / saved-chain restore) on
// top of an engine that already has a live chain — the native chain survives
// renderer re-evaluations, so an unconditional restore appended an exact
// duplicate of every stage (two amp stages in series = "gain blown out").

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const vm = require('node:vm');

const ROOT = path.join(__dirname, '..');
const SCREEN_JS = fs.readFileSync(path.join(ROOT, 'src', 'renderer', 'screen.js'), 'utf8');

// Brace-balanced extraction of `function NAME(...) { ... }`.
function extractFunction(src, name) {
    const sig = `function ${name}(`;
    const start = src.indexOf(sig);
    assert.ok(start !== -1, `function '${name}' not found`);
    const openBrace = src.indexOf('{', start);
    let depth = 1;
    let i = openBrace + 1;
    while (i < src.length && depth > 0) {
        if (src[i] === '{') depth++;
        else if (src[i] === '}') depth--;
        i++;
    }
    assert.ok(depth === 0, `unbalanced braces in '${name}'`);
    return src.slice(start, i);
}

// Extract the marker const + both helpers into one sandbox.
function setupSandbox() {
    const markerStart = SCREEN_JS.indexOf('const RB_PLUMBING_MARKER');
    assert.ok(markerStart !== -1, 'RB_PLUMBING_MARKER not found');
    const markerDecl = SCREEN_JS.slice(markerStart, SCREEN_JS.indexOf(';', markerStart) + 1);
    const code = [
        markerDecl,
        extractFunction(SCREEN_JS, 'isRigBuilderChainStage'),
        extractFunction(SCREEN_JS, 'saveChainStateFromChain'),
    ].join('\n');
    const stored = new Map();
    const sandbox = {
        localStorage: {
            getItem: (k) => (stored.has(k) ? stored.get(k) : null),
            setItem: (k, v) => { stored.set(k, String(v)); },
        },
    };
    vm.runInNewContext(code, sandbox, { filename: 'chain-persistence.js' });
    return { sandbox, stored };
}

const RB_CHAIN = [
    { type: 0, path: '/vst/SamplegSBTCL.vst3', name: 'SamplegSBTCL' },
    { type: 2, path: '/irs/_rb_unit_impulse.wav', name: '_rb_unit_impulse' },
    { type: 0, path: '/vst/RB Final Leveler.vst3', name: 'RB Final Leveler' },
];

test('saveChainStateFromChain skips a Rig-Builder-owned live chain', () => {
    const { sandbox, stored } = setupSandbox();
    stored.set('slopsmith-signal-chain', '[{"type":"NAM","path":"/nam/amp.nam","name":"amp"}]');
    sandbox.saveChainStateFromChain([...RB_CHAIN, { type: 1, path: '/nam/vox.nam', name: 'VOX' }]);
    assert.equal(
        stored.get('slopsmith-signal-chain'),
        '[{"type":"NAM","path":"/nam/amp.nam","name":"amp"}]',
        'the previously saved panel-built chain must be preserved',
    );
});

test('saveChainStateFromChain persists a panel-built chain normally', () => {
    const { sandbox, stored } = setupSandbox();
    sandbox.saveChainStateFromChain([
        { type: 0, path: '/vst/MyAmp.vst3', name: 'MyAmp' },
        { type: 1, path: '/nam/vox.nam', name: 'VOX' },
        { type: 7, path: '/x', name: 'not-a-chain-type' },
    ]);
    assert.deepEqual(JSON.parse(stored.get('slopsmith-signal-chain')), [
        { type: 'VST', path: '/vst/MyAmp.vst3', name: 'MyAmp' },
        { type: 'NAM', path: '/nam/vox.nam', name: 'VOX' },
    ]);
});

test('isRigBuilderChainStage matches plumbing by path or name only', () => {
    const { sandbox } = setupSandbox();
    assert.equal(sandbox.isRigBuilderChainStage({ path: '/irs/_rb_unit_impulse.wav' }), true);
    assert.equal(sandbox.isRigBuilderChainStage({ name: 'RB Final Leveler' }), true);
    assert.equal(sandbox.isRigBuilderChainStage({ path: '/vst/MyAmp.vst3', name: 'MyAmp' }), false);
    assert.equal(sandbox.isRigBuilderChainStage(null), false);
});

test('init auto-load is gated on an empty engine chain (re-evaluation guard)', () => {
    // Structural assertions: the guard exists and both auto-load paths honor it.
    assert.equal(SCREEN_JS.includes('_chainAlreadyLive'), true);
    assert.equal(
        SCREEN_JS.includes('if (!_chainAlreadyLive && _ampSimsEnabled && !_defaultLoaded)'),
        true,
        'saved-chain restore must be gated on the live-chain probe',
    );
    assert.equal(
        SCREEN_JS.includes('if (_chainAlreadyLive) {'),
        true,
        'default-preset auto-load must be gated on the live-chain probe',
    );
});

test('restore drops Rig Builder plumbing stages from legacy saves', () => {
    // aeRestoreSavedChain is async and coupled to the api bridge; assert the
    // sanitize step is present and rewrites the cleaned save.
    const fn = extractFunction(SCREEN_JS, 'aeRestoreSavedChain');
    assert.equal(fn.includes('isRigBuilderChainStage'), true);
    assert.equal(fn.includes("localStorage.setItem('slopsmith-signal-chain', JSON.stringify(_cleaned))"), true);
});
