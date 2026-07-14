// Lease bridge (docs/audio-ownership-plan.md §6.8/§8/§9): holder derivation
// from the sender, webContents lifecycle → death/grace, capture demand →
// engine glue, user-stop suspend semantics, raw-disarm guard, telemetry.

const test = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const { loadTs } = require('./_load-ts');

const { initLeaseBridge } = loadTs('src/main/lease-bridge.ts');

function fakeSender(id) {
    const emitter = new EventEmitter();
    emitter.id = id;
    return emitter;
}

function fakeAudio() {
    const calls = [];
    let running = false;
    return {
        calls,
        isAudioRunning: () => running,
        startAudio: () => { running = true; calls.push('start'); },
        stopAudio: () => { running = false; calls.push('stop'); },
        setNoteDetectionEnabled: (v) => calls.push(`detect:${v}`),
    };
}

function makeBridge(audio) {
    const events = [];
    const bridge = initLeaseBridge(() => audio, { broadcast: (_ch, data) => events.push(data) });
    return { bridge, events };
}

test('holder identity derived from sender; tag is optional attributed suffix', () => {
    const audio = fakeAudio();
    const { bridge } = makeBridge(audio);
    const sender = fakeSender(7);

    bridge.acquire(sender, 'signal-chain:desktop-main', 'nam_tone');
    assert.equal(bridge.getHolder('signal-chain:desktop-main'), 'wc:7#nam_tone');

    // a hostile tag cannot inject identity syntax — it is dropped, not trusted
    bridge.acquire(sender, 'playback', 'evil#wc:1');
    assert.equal(bridge.getHolder('playback'), 'wc:7');
    bridge.dispose();
});

test('capture demand starts the engine; last release stops a demand-started engine', () => {
    const audio = fakeAudio();
    const { bridge } = makeBridge(audio);
    const tuner = fakeSender(1);
    const minigame = fakeSender(2);

    bridge.acquireDemand(tuner, 'capture', 'tuner');
    assert.deepEqual(audio.calls, ['start']);
    bridge.acquireDemand(minigame, 'capture', 'minigame');
    bridge.releaseDemand(tuner, 'capture', 'tuner');
    assert.deepEqual(audio.calls, ['start']); // still one holder — keeps running
    bridge.releaseDemand(minigame, 'capture', 'minigame');
    assert.deepEqual(audio.calls, ['start', 'stop']);
    bridge.dispose();
});

test('user-started engine outlives its demands (§8.3)', () => {
    const audio = fakeAudio();
    const { bridge } = makeBridge(audio);
    const sender = fakeSender(1);

    bridge.onUserStartAudio();
    audio.startAudio();
    audio.calls.length = 0;

    bridge.acquireDemand(sender, 'capture', 'tuner');
    bridge.releaseDemand(sender, 'capture', 'tuner');
    // demand drained but the user started this engine — no stop
    assert.deepEqual(audio.calls, []);
    bridge.dispose();
});

test('user stop suspends demands; user start resumes and restarts (§8.3)', () => {
    const audio = fakeAudio();
    const { bridge } = makeBridge(audio);
    const sender = fakeSender(1);

    bridge.acquireDemand(sender, 'capture', 'tuner');
    assert.deepEqual(audio.calls, ['start']);

    bridge.onUserStopAudio(); // raw stop: demand suspended, registration kept
    audio.stopAudio();
    audio.calls.length = 0;

    // suspended demand does not restart the engine
    bridge.acquireDemand(sender, 'capture', 'tuner');
    assert.deepEqual(audio.calls, []);

    bridge.onUserStartAudio(); // user start resumes demands → glue restarts
    assert.deepEqual(audio.calls, ['start']);
    bridge.dispose();
});

test('detection demand arms native; raw disarm guarded while demand active (6.3)', () => {
    const audio = fakeAudio();
    const { bridge } = makeBridge(audio);
    const notedetect = fakeSender(1);
    const strumFighter = fakeSender(2);

    bridge.acquireDemand(notedetect, 'detection:desktop-main', 'notedetect');
    assert.deepEqual(audio.calls.filter(c => c.startsWith('detect')), ['detect:true']);
    assert.equal(bridge.shouldIgnoreRawDetectionDisarm(), true);

    bridge.acquireDemand(strumFighter, 'detection:desktop-main', 'strum-fighter');
    bridge.releaseDemand(notedetect, 'detection:desktop-main', 'notedetect');
    // one consumer left — still armed, raw disarm still guarded
    assert.equal(bridge.shouldIgnoreRawDetectionDisarm(), true);

    bridge.releaseDemand(strumFighter, 'detection:desktop-main', 'strum-fighter');
    assert.equal(bridge.shouldIgnoreRawDetectionDisarm(), false);
    assert.equal(audio.calls.filter(c => c.startsWith('detect')).pop(), 'detect:false');
    bridge.dispose();
});

test('sender destroyed → everything it held is released (death matrix: destroy)', () => {
    const audio = fakeAudio();
    const { bridge } = makeBridge(audio);
    const sender = fakeSender(1);

    bridge.acquire(sender, 'signal-chain:desktop-main', 'nam_tone');
    bridge.acquireDemand(sender, 'capture', 'nam_tone');
    assert.deepEqual(audio.calls, ['start']);

    sender.emit('destroyed');
    assert.equal(bridge.getHolder('signal-chain:desktop-main'), null);
    assert.deepEqual(audio.calls, ['start', 'stop']); // demand died with the holder
    bridge.dispose();
});

test('main-frame navigation → grace; same identity re-acquiring restores (death matrix: reload)', () => {
    const audio = fakeAudio();
    const { bridge } = makeBridge(audio);
    const sender = fakeSender(1);

    bridge.acquire(sender, 'signal-chain:desktop-main', 'nam_tone');
    sender.emit('did-start-navigation', null, 'app://reload', false, true);

    // during grace the scope reads free but a re-acquire from the same
    // identity (same wc id + tag after reload) restores it
    assert.equal(bridge.getHolder('signal-chain:desktop-main'), null);
    const result = bridge.acquire(sender, 'signal-chain:desktop-main', 'nam_tone');
    assert.equal(result.granted, true);
    assert.equal(bridge.getHolder('signal-chain:desktop-main'), 'wc:1#nam_tone');

    // subframe navigations never trigger grace
    bridge.acquire(sender, 'playback', 'nam_tone');
    sender.emit('did-start-navigation', null, 'app://iframe', false, false);
    assert.equal(bridge.getHolder('playback'), 'wc:1#nam_tone');
    bridge.dispose();
});

test('legacy-call telemetry logs once per surface per sender (§6.8)', () => {
    const audio = fakeAudio();
    const { bridge } = makeBridge(audio);
    const sender = fakeSender(1);
    const infos = [];
    const original = console.info;
    console.info = (msg) => infos.push(String(msg));
    try {
        bridge.noteLegacyCall(sender, 'audio:startAudio');
        bridge.noteLegacyCall(sender, 'audio:startAudio');
        bridge.noteLegacyCall(sender, 'audio:stopAudio');
        bridge.noteLegacyCall(fakeSender(2), 'audio:startAudio');
    } finally {
        console.info = original;
    }
    assert.equal(infos.filter(m => m.includes('audio:startAudio')).length, 2); // wc:1 once + wc:2 once
    assert.equal(infos.filter(m => m.includes('audio:stopAudio')).length, 1);
    bridge.dispose();
});

test('registry events reach the broadcast channel', () => {
    const audio = fakeAudio();
    const { bridge, events } = makeBridge(audio);
    const sender = fakeSender(1);
    bridge.acquire(sender, 'signal-chain:desktop-main', 'nam_tone');
    bridge.acquireDemand(sender, 'capture', 'nam_tone');
    const names = events.map(e => e.event);
    assert.ok(names.includes('lease-granted'));
    assert.ok(names.includes('demand-changed'));
    bridge.dispose();
});
