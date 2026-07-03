// Guards against the alpha-tester chain-duplication/pollution bugs: the
// audio_engine panel must (1) persist only the USER's stages — never Rig
// Builder's always-on tone stages — into localStorage and named presets,
// (2) drop Rig Builder stages from legacy saves on restore, and (3) never
// auto-load (default preset / saved-chain restore) on top of an engine that
// already has a live chain — the native chain survives renderer
// re-evaluations, so an unconditional restore appended an exact duplicate of
// every stage (two amp stages in series = "gain blown out").

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
const vm = require('node:vm');

const ROOT = path.join(__dirname, '..');
const SCREEN_JS = fs.readFileSync(path.join(ROOT, 'src', 'renderer', 'screen.js'), 'utf8');

// Brace-balanced extraction of `function NAME(...) { ... }`. Skips past the
// parameter list first (paren-balanced) so a destructured default parameter
// like `{ snapshot = true } = {}` isn't mistaken for the body's opening brace.
function extractFunction(src, name) {
    const sig = `function ${name}(`;
    const start = src.indexOf(sig);
    assert.ok(start !== -1, `function '${name}' not found`);
    let i = start + sig.length;
    let parens = 1;
    while (i < src.length && parens > 0) {
        if (src[i] === '(') parens++;
        else if (src[i] === ')') parens--;
        i++;
    }
    assert.ok(parens === 0, `unbalanced parens in '${name}' signature`);
    const openBrace = src.indexOf('{', i);
    let depth = 1;
    i = openBrace + 1;
    while (i < src.length && depth > 0) {
        if (src[i] === '{') depth++;
        else if (src[i] === '}') depth--;
        i++;
    }
    assert.ok(depth === 0, `unbalanced braces in '${name}'`);
    return src.slice(start, i);
}

function setupSandbox() {
    const code = [
        extractFunction(SCREEN_JS, 'aeIsRigBuilderStage'),
        extractFunction(SCREEN_JS, 'aeStripRigBuilderFromNativePreset'),
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
    { type: 0, path: '/plugins/rig_builder/vst/SamplegSBTCL.vst3', name: 'SamplegSBTCL' },
    { type: 2, path: '/irs/_rb_unit_impulse.wav', name: '_rb_unit_impulse' },
    { type: 0, path: '/vst/RB Final Leveler.vst3', name: 'RB Final Leveler' },
];

test('saveChainStateFromChain persists only the user stages of a mixed chain', () => {
    const { sandbox, stored } = setupSandbox();
    sandbox.saveChainStateFromChain([...RB_CHAIN, { type: 1, path: '/nam/vox.nam', name: 'VOX' }]);
    assert.deepEqual(JSON.parse(stored.get('slopsmith-signal-chain')), [
        { type: 'NAM', path: '/nam/vox.nam', name: 'VOX' },
    ], 'RB stages stripped, the user NAM stacked on top survives');
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

test('aeIsRigBuilderStage recognizes every Rig Builder stage shape', () => {
    const { sandbox } = setupSandbox();
    const rb = sandbox.aeIsRigBuilderStage;
    // Bundled RB gear by plugin-dir path (amps/pedals/racks), both separators.
    assert.equal(rb({ path: '/plugins/rig_builder/vst/SamplegSBTCL.vst3' }), true);
    assert.equal(rb({ path: 'C:\\plugins\\rig_builder\\vst\\Amp_AT20.vst3' }), true);
    // Plumbing by name or path, wherever the file lives.
    assert.equal(rb({ path: '/irs/_rb_unit_impulse.wav' }), true);
    assert.equal(rb({ name: '_rb_unit_impulse' }), true);
    assert.equal(rb({ name: 'RB Final Leveler' }), true);
    assert.equal(rb({ path: '/vst/RB Final Leveler.vst3' }), true);
    // Backend chain-spec sentinels.
    assert.equal(rb({ rs_gear: '__rb_final_leveler__' }), true);
    assert.equal(rb({ slot: 'master_pre' }), true);
    assert.equal(rb({ slot: 'master_post' }), true);
    // User gear is untouched.
    assert.equal(rb({ path: '/vst/MyAmp.vst3', name: 'MyAmp' }), false);
    assert.equal(rb({ path: '/nam/vox.nam', name: 'VOX' }), false);
    assert.equal(rb(null), false);
});

test('aeStripRigBuilderFromNativePreset filters the blob chain, keeps user state', () => {
    const { sandbox } = setupSandbox();
    const blob = JSON.stringify({
        chain: [
            { type: 0, path: '/plugins/rig_builder/vst/SamplegSBTCL.vst3', state: 'rb' },
            { type: 0, path: '/vst/MyReverb.vst3', state: 'user-params' },
            { type: 0, path: '/vst/x.vst3', slot: 'master_post', state: 'rb' },
        ],
        gains: { input: 1 },
    });
    const out = JSON.parse(sandbox.aeStripRigBuilderFromNativePreset(blob));
    assert.deepEqual(out.chain, [{ type: 0, path: '/vst/MyReverb.vst3', state: 'user-params' }]);
    assert.deepEqual(out.gains, { input: 1 }, 'non-chain fields pass through');
    // Unparseable blob passes through untouched.
    assert.equal(sandbox.aeStripRigBuilderFromNativePreset('not json{'), 'not json{');
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

test('restore self-heals legacy saves and preset save/load strip RB stages', () => {
    const restore = extractFunction(SCREEN_JS, 'aeRestoreSavedChain');
    assert.equal(restore.includes('aeIsRigBuilderStage'), true);
    assert.equal(restore.includes("localStorage.setItem('slopsmith-signal-chain', JSON.stringify(_cleaned))"), true);
    // Save Current Chain strips both the native blob and the item list…
    assert.equal(SCREEN_JS.includes('aeStripRigBuilderFromNativePreset(nativePresetRaw)'), true);
    // …and the preset-load path sanitizes legacy polluted presets.
    const load = extractFunction(SCREEN_JS, 'replaceChainWithPresetBlob');
    assert.equal(load.includes('aeStripRigBuilderFromNativePreset(preset.nativePreset)'), true);
});
