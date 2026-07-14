const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const Module = require('node:module');
const ts = require('typescript');

const ROOT = path.join(__dirname, '..');
const EXECUTOR_TS = path.join(ROOT, 'src', 'main', 'audio-effects-executor.ts');

function loadExecutorModule() {
    const source = fs.readFileSync(EXECUTOR_TS, 'utf8');
    const compiled = ts.transpileModule(source, {
        compilerOptions: {
            module: ts.ModuleKind.CommonJS,
            target: ts.ScriptTarget.ES2022,
            esModuleInterop: true,
        },
        fileName: EXECUTOR_TS,
    }).outputText;
    const mod = new Module(EXECUTOR_TS, module);
    mod.filename = EXECUTOR_TS;
    mod.paths = Module._nodeModulePaths(path.dirname(EXECUTOR_TS));
    mod._compile(compiled, EXECUTOR_TS);
    return mod.exports;
}

function tempAsset(ext) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'slopsmith-audio-effects-'));
    const file = path.join(dir, `asset${ext}`);
    fs.writeFileSync(file, 'test');
    return file;
}

function plan(overrides = {}) {
    return {
        schema: 'slopsmith.audio_effects.chain_plan.v1',
        planId: 'plan-1',
        routeKey: 'desktop-main',
        providerId: 'rig-builder',
        stages: [
            { stageId: 'pre', kind: 'nam', role: 'pre-pedal', assetRef: 'asset:pre' },
            { stageId: 'cab', kind: 'ir', role: 'cab', assetRef: 'asset:cab', bypassed: true },
        ],
        segments: [{ segmentId: 'lead', stageIds: ['pre'], stageBypass: { pre: true } }],
        ...overrides,
    };
}

test('audio-effects executor validates and loads a trusted chain plan without leaking asset paths', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    const calls = [];
    const native = {
        loadPreset: async presetJson => {
            const parsed = JSON.parse(presetJson);
            calls.push(parsed.chain);
            return { success: true, slotsLoaded: parsed.chain.length };
        },
        getChainState: () => [{ id: 10 }, { id: 11 }],
        setMultiBypass: changes => { calls.push(['multi', changes]); return true; },
        setBypass: (slotId, bypassed) => { calls.push(['bypass', slotId, bypassed]); return true; },
        setParameter: (slotId, paramIndex, value) => { calls.push(['param', slotId, paramIndex, value]); return true; },
    };
    const executor = createAudioEffectsExecutor(() => native);
    const namPath = tempAsset('.nam');
    const irPath = tempAsset('.wav');

    const loaded = await executor.loadChainPlan({
        authorization: 'playback-session',
        plan: plan(),
        assets: {
            'asset:pre': { kind: 'nam', path: namPath, safeName: 'pre' },
            'asset:cab': { kind: 'ir', path: irPath, safeName: 'cab' },
        },
    });
    const inspected = executor.inspectRoute('desktop-main');
    const segment = await executor.activateSegment({ routeKey: 'desktop-main', segmentId: 'lead' });
    const bypass = await executor.setStageBypass({ routeKey: 'desktop-main', stageId: 'pre', bypassed: true });
    const param = await executor.setStageParameter({ routeKey: 'desktop-main', stageId: 'pre', paramIndex: 2, value: 0.75 });
    const encoded = JSON.stringify({ loaded, inspected, segment, bypass, param });

    assert.equal(loaded.outcome, 'handled');
    assert.equal(inspected.payload.route.nativeStageCount, 2);
    assert.equal(segment.outcome, 'handled');
    assert.equal(bypass.outcome, 'handled');
    assert.equal(param.outcome, 'handled');
    assert.deepEqual(calls[0].map(stage => stage.type), [1, 2]);
    assert.deepEqual(calls[1], ['multi', [{ slotId: 10, bypassed: true }, { slotId: 11, bypassed: true }]]);
    assert.equal(encoded.includes(namPath), false);
    assert.equal(encoded.includes(irPath), false);
    assert.equal(encoded.includes('asset:pre'), false);
});

test('audio-effects executor rejects a plan with duplicate segmentIds', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    const native = {
        loadPreset: async () => ({ success: true, slotsLoaded: 2 }),
        getChainState: () => [{ id: 10 }, { id: 11 }],
    };
    const executor = createAudioEffectsExecutor(() => native);
    const namPath = tempAsset('.nam');
    const irPath = tempAsset('.wav');
    const loaded = await executor.loadChainPlan({
        authorization: 'playback-session',
        plan: plan({ segments: [
            { segmentId: 'lead', stageIds: ['pre'] },
            { segmentId: 'lead', stageIds: ['cab'] },
        ] }),
        assets: {
            'asset:pre': { kind: 'nam', path: namPath, safeName: 'pre' },
            'asset:cab': { kind: 'ir', path: irPath, safeName: 'cab' },
        },
    });
    assert.equal(loaded.outcome, 'failed');
    assert.equal(JSON.stringify(loaded.payload.errors).includes('duplicate segmentId'), true);
});

test('audio-effects executor returns failed (with rollback) when native chain-state lookup throws', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    const native = {
        savePreset: () => 'previous',
        loadPreset: async () => ({ success: true, slotsLoaded: 2 }),
        getChainState: () => { throw new Error('native chain-state boom'); },
    };
    const executor = createAudioEffectsExecutor(() => native);
    const namPath = tempAsset('.nam');
    const irPath = tempAsset('.wav');
    const loaded = await executor.loadChainPlan({
        authorization: 'playback-session',
        plan: plan(),
        assets: {
            'asset:pre': { kind: 'nam', path: namPath, safeName: 'pre' },
            'asset:cab': { kind: 'ir', path: irPath, safeName: 'cab' },
        },
    });
    assert.equal(loaded.outcome, 'failed');
    assert.equal(loaded.payload.rollbackApplied, true);
    // The route must not be registered when the lookup failed.
    assert.equal(executor.inspectRoute('desktop-main').outcome, 'no-target');
});

test('audio-effects executor rolls back to degraded when native slot mapping is incomplete', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    const native = {
        savePreset: () => 'previous',
        loadPreset: async () => ({ success: true, slotsLoaded: 2 }),
        // loadPreset reports both stages loaded, but the chain state only maps one valid slot.
        getChainState: () => [{ id: 10 }, { id: -1 }],
    };
    const executor = createAudioEffectsExecutor(() => native);
    const namPath = tempAsset('.nam');
    const irPath = tempAsset('.wav');
    const loaded = await executor.loadChainPlan({
        authorization: 'playback-session',
        plan: plan(),
        assets: {
            'asset:pre': { kind: 'nam', path: namPath, safeName: 'pre' },
            'asset:cab': { kind: 'ir', path: irPath, safeName: 'cab' },
        },
    });
    assert.equal(loaded.outcome, 'degraded');
    assert.equal(loaded.payload.slotsMapped, 1);
    assert.equal(executor.inspectRoute('desktop-main').outcome, 'no-target');
});

test('audio-effects executor owns load mute, route gain, start, and release', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    const calls = [];
    const native = {
        savePreset: () => 'previous',
        loadPreset: presetJson => { calls.push(['load', JSON.parse(presetJson).chain.length]); return { success: true, slotsLoaded: 2 }; },
        clearChain: () => { calls.push(['clear']); return true; },
        getChainState: () => [{ id: 10 }, { id: 11 }],
        isMonitorMuted: () => { calls.push(['is-muted']); return true; },
        setMonitorMute: muted => { calls.push(['monitor', muted]); return true; },
        setMonitorMuteSuppressed: suppressed => { calls.push(['suppress', suppressed]); return true; },
        setGain: (which, value) => { calls.push(['gain', which, value]); return true; },
        startAudio: () => { calls.push(['start']); return true; },
    };
    const executor = createAudioEffectsExecutor(() => native);
    const namPath = tempAsset('.nam');
    const irPath = tempAsset('.wav');

    const loaded = await executor.loadChainPlan({
        authorization: 'playback-session',
        plan: plan(),
        assets: {
            'asset:pre': { kind: 'nam', path: namPath, safeName: 'pre' },
            'asset:cab': { kind: 'ir', path: irPath, safeName: 'cab' },
        },
        options: { preloadMute: { targetGain: 4, holdMs: 0 }, gains: { input: 8, chain: 4 }, startAudio: true },
    });
    const gained = await executor.setRouteGain({ routeKey: 'desktop-main', gains: { chain: 2 } });
    const released = await executor.releaseRoute({ routeKey: 'desktop-main' });
    const inspected = executor.inspectRoute('desktop-main');
    await new Promise(resolve => setTimeout(resolve, 40));

    assert.equal(loaded.outcome, 'handled');
    assert.equal(gained.outcome, 'handled');
    assert.equal(released.outcome, 'handled');
    assert.equal(inspected.outcome, 'no-target');
    assert.deepEqual(calls.slice(0, 7), [
        ['is-muted'],
        ['gain', 'chain', 0],
        ['monitor', false],
        ['load', 2],
        ['gain', 'input', 8],
        ['start'],
        ['gain', 'chain', 2],
    ]);
    assert.equal(calls.some(call => call[0] === 'clear'), true);
    assert.equal(calls.some(call => call[0] === 'monitor' && call[1] === true), true);
    assert.equal(calls.some(call => call[0] === 'suppress' && call[1] === false), true);
    assert.equal(calls.some(call => call[0] === 'gain' && call[1] === 'chain' && call[2] === 4), false);
    assert.equal(calls.some(call => call[0] === 'gain' && call[1] === 'chain' && call[2] === 0), true);
});

test('audio-effects executor rejects unauthorised, missing, and raw-path-like plans before native load', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    let loadCount = 0;
    const executor = createAudioEffectsExecutor(() => ({
        loadPreset: () => { loadCount += 1; return { success: true, slotsLoaded: 1 }; },
        getChainState: () => [{ id: 1 }],
    }));

    const noAuth = await executor.loadChainPlan({ plan: plan(), assets: {} });
    const missingAsset = await executor.loadChainPlan({ authorization: 'user-action', plan: plan(), assets: {} });
    const rawPath = await executor.loadChainPlan({
        authorization: 'user-action',
        plan: plan({ stages: [{ stageId: 'amp', kind: 'nam', role: 'amp', assetRef: '/Users/example/private/model.nam' }] }),
        assets: {},
    });
    const encoded = JSON.stringify({ noAuth, missingAsset, rawPath });

    assert.equal(noAuth.outcome, 'failed');
    assert.equal(missingAsset.outcome, 'failed');
    assert.equal(rawPath.outcome, 'failed');
    assert.equal(loadCount, 0);
    assert.equal(encoded.includes('/Users/example'), false);
    assert.equal(encoded.includes('model.nam'), false);
});

test('audio-effects executor rejects duplicate stage ids before native load', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    let loadCount = 0;
    const namPath = tempAsset('.nam');
    const irPath = tempAsset('.wav');
    const executor = createAudioEffectsExecutor(() => ({
        loadPreset: () => { loadCount += 1; return { success: true, slotsLoaded: 2 }; },
        getChainState: () => [{ id: 1 }, { id: 2 }],
    }));

    const result = await executor.loadChainPlan({
        authorization: 'user-action',
        plan: plan({
            stages: [
                { stageId: 'dup', kind: 'nam', role: 'amp', assetRef: 'asset:pre' },
                { stageId: 'dup', kind: 'ir', role: 'cab', assetRef: 'asset:cab' },
            ],
        }),
        assets: {
            'asset:pre': { kind: 'nam', path: namPath, safeName: 'pre' },
            'asset:cab': { kind: 'ir', path: irPath, safeName: 'cab' },
        },
    });

    assert.equal(result.outcome, 'failed');
    assert.equal(loadCount, 0);
    assert.equal(JSON.stringify(result).includes('Duplicate stageId dup'), true);
});

test('audio-effects executor rolls back and avoids route state on partial native loads', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    const calls = [];
    const executor = createAudioEffectsExecutor(() => ({
        savePreset: () => 'previous-preset',
        loadPreset: presetJson => {
            calls.push(presetJson);
            return { success: true, slotsLoaded: 1 };
        },
        getChainState: () => [{ id: 10 }],
    }));
    const namPath = tempAsset('.nam');
    const irPath = tempAsset('.wav');

    const loaded = await executor.loadChainPlan({
        authorization: 'playback-session',
        plan: plan(),
        assets: {
            'asset:pre': { kind: 'nam', path: namPath, safeName: 'pre' },
            'asset:cab': { kind: 'ir', path: irPath, safeName: 'cab' },
        },
    });
    const inspected = executor.inspectRoute('desktop-main');
    const encoded = JSON.stringify(loaded);

    assert.equal(loaded.outcome, 'degraded');
    assert.equal(loaded.payload.rollbackApplied, true);
    assert.equal(inspected.outcome, 'no-target');
    assert.equal(calls.length, 2);
    assert.equal(calls[1], 'previous-preset');
    assert.equal(encoded.includes(namPath), false);
    assert.equal(encoded.includes(irPath), false);
});

test('audio-effects executor reports native control failures without throwing IPC errors', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    const namPath = tempAsset('.nam');
    const irPath = tempAsset('.wav');
    const executor = createAudioEffectsExecutor(() => ({
        loadPreset: () => ({ success: true, slotsLoaded: 2 }),
        getChainState: () => [{ id: 10 }, { id: 11 }],
        setBypass: async () => { throw new Error('plugin crash /Users/example/private.nam'); },
        setParameter: async () => ({ success: false }),
        setMultiBypass: async () => false,
    }));

    const loaded = await executor.loadChainPlan({
        authorization: 'playback-session',
        plan: plan(),
        assets: {
            'asset:pre': { kind: 'nam', path: namPath, safeName: 'pre' },
            'asset:cab': { kind: 'ir', path: irPath, safeName: 'cab' },
        },
    });
    const bypass = await executor.setStageBypass({ routeKey: 'desktop-main', stageId: 'pre', bypassed: true });
    const param = await executor.setStageParameter({ routeKey: 'desktop-main', stageId: 'pre', paramIndex: 2, value: 0.75 });
    const segment = await executor.activateSegment({ routeKey: 'desktop-main', segmentId: 'lead' });
    const encoded = JSON.stringify({ bypass, param, segment });

    assert.equal(loaded.outcome, 'handled');
    assert.equal(bypass.outcome, 'failed');
    assert.equal(bypass.reason, 'Native stage bypass threw');
    assert.equal(param.outcome, 'failed');
    assert.equal(param.reason, 'Native stage parameter returned failure');
    assert.equal(segment.outcome, 'failed');
    assert.equal(segment.reason, 'Native multi-bypass returned failure');
    assert.equal(encoded.includes('/Users/example'), false);
    assert.equal(encoded.includes('private.nam'), false);
});

test('audio-effects executor rejects coerced parameter indices', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    const namPath = tempAsset('.nam');
    const irPath = tempAsset('.wav');
    const calls = [];
    const executor = createAudioEffectsExecutor(() => ({
        loadPreset: () => ({ success: true, slotsLoaded: 2 }),
        getChainState: () => [{ id: 10 }, { id: 11 }],
        setParameter: (...args) => { calls.push(args); return true; },
    }));

    await executor.loadChainPlan({
        authorization: 'playback-session',
        plan: plan(),
        assets: {
            'asset:pre': { kind: 'nam', path: namPath, safeName: 'pre' },
            'asset:cab': { kind: 'ir', path: irPath, safeName: 'cab' },
        },
    });

    assert.equal((await executor.setStageParameter({ routeKey: 'desktop-main', stageId: 'pre', paramIndex: false, value: 0.75 })).outcome, 'failed');
    assert.equal((await executor.setStageParameter({ routeKey: 'desktop-main', stageId: 'pre', paramIndex: '', value: 0.75 })).outcome, 'failed');
    assert.equal((await executor.setStageParameter({ routeKey: 'desktop-main', stageId: 'pre', paramIndex: 4096, value: 0.75 })).outcome, 'failed');
    assert.equal((await executor.setStageParameter({ routeKey: 'desktop-main', stageId: 'pre', paramIndex: '2', value: '0.75' })).outcome, 'handled');
    assert.deepEqual(calls, [[10, 2, 0.75]]);
});

test('preload exposes the trusted audio-effects executor surface', () => {
    const preload = fs.readFileSync(path.join(ROOT, 'src', 'main', 'preload.ts'), 'utf8');
    const bridge = fs.readFileSync(path.join(ROOT, 'src', 'main', 'audio-bridge.ts'), 'utf8');

    assert.equal(preload.includes('audioEffects: {'), true);
    for (const method of ['loadChainPlan', 'releaseRoute', 'inspectRoute', 'activateSegment', 'setStageBypass', 'setStageParameter', 'setRouteGain']) {
        assert.equal(preload.includes(`${method}:`), true);
    }
    for (const channel of [
        'audio-effects:loadChainPlan',
        'audio-effects:releaseRoute',
        'audio-effects:inspectRoute',
        'audio-effects:activateSegment',
        'audio-effects:setStageBypass',
        'audio-effects:setStageParameter',
        'audio-effects:setRouteGain',
    ]) {
        assert.equal(bridge.includes(channel), true);
    }
    assert.equal(bridge.includes("ipcMain.handle('audio-effects:loadChainPlan', async"), true);
    assert.equal(bridge.includes('vstSlotPaths.clear();\n        return await audioEffects.loadChainPlan(request);'), true);
    assert.equal(bridge.includes('if (normalizedPayload.inputType !== normalizedPayload.outputType)'), true);
});

test('audio-effects executor detects a foreign chain write via chainGeneration and reports a stale route', async () => {
    const { createAudioEffectsExecutor } = loadExecutorModule();
    // Native stub with the phase-7a generation counter: our load lands at
    // generation 5; a foreign writer (legacy loadPreset / clearChain) later
    // bumps it to 6, invalidating the route's stageSlots map.
    let generation = 5;
    const bypassCalls = [];
    const native = {
        loadPreset: async presetJson => ({ success: true, slotsLoaded: JSON.parse(presetJson).chain.length, chainGeneration: generation }),
        getChainState: () => [{ id: 10 }, { id: 11 }],
        getChainGeneration: () => generation,
        setBypass: (slotId, bypassed) => { bypassCalls.push([slotId, bypassed]); return true; },
        setMultiBypass: changes => { bypassCalls.push(['multi', changes]); return true; },
        setParameter: () => true,
    };
    const executor = createAudioEffectsExecutor(() => native);
    const loaded = await executor.loadChainPlan({
        authorization: 'playback-session',
        plan: plan(),
        assets: {
            'asset:pre': { kind: 'nam', path: tempAsset('.nam'), safeName: 'pre' },
            'asset:cab': { kind: 'ir', path: tempAsset('.wav'), safeName: 'cab' },
        },
    });
    assert.equal(loaded.outcome, 'handled');

    // Generation unchanged: stage ops flow normally.
    const fresh = await executor.setStageBypass({ routeKey: 'desktop-main', stageId: 'pre', bypassed: true });
    assert.equal(fresh.outcome, 'handled');
    assert.equal(bypassCalls.length, 1);

    // Foreign write bumps the native counter.
    generation = 6;
    const stale = await executor.setStageBypass({ routeKey: 'desktop-main', stageId: 'pre', bypassed: false });
    assert.equal(stale.outcome, 'no-target');
    assert.match(stale.reason, /modified by another writer/);
    assert.equal(stale.payload.expectedGeneration, 5);
    assert.equal(stale.payload.currentGeneration, 6);
    assert.equal(bypassCalls.length, 1, 'stale route must NOT touch native slots');

    // Segment activation and parameters are equally guarded.
    const seg = await executor.activateSegment({ routeKey: 'desktop-main', segmentId: 'lead' });
    assert.equal(seg.outcome, 'no-target');
    const param = await executor.setStageParameter({ routeKey: 'desktop-main', stageId: 'pre', paramIndex: 0, value: 0.5 });
    assert.equal(param.outcome, 'no-target');
    assert.equal(bypassCalls.length, 1);
});
