// Mixer bridge (plan §5.1/§8): tier-3 producer-handle enforcement, gain
// persistence keyed holderId+label, idle reap, double-audio heuristic,
// holder-death cleanup, tier-1/2 open surfaces.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { EventEmitter } = require('node:events');
const { loadTs } = require('./_load-ts');

const { initMixerBridge } = loadTs('src/main/mixer-bridge.ts');

function fakeSender(id) {
    const emitter = new EventEmitter();
    emitter.id = id;
    return emitter;
}

// Minimal native-mixer fake mirroring the addon surface.
function fakeNativeMixer() {
    const channels = new Map([[0, { id: 0, label: 'renderer-master', kind: 'default', holder: 'engine:renderer-default', gain: 1, mute: false, group: -1, enabled: false, fillFrames: 0, pushedFrames: 0, consumedFrames: 0, underflowCount: 0, overflowCount: 0 }]]);
    let nextId = 1;
    return {
        channels,
        mixerCreateChannel: (label, kind, holder) => {
            if (nextId >= 24) return -1;
            const id = nextId++;
            channels.set(id, { id, label, kind, holder, gain: 1, mute: false, group: -1, enabled: true, fillFrames: 0, pushedFrames: 0, consumedFrames: 0, underflowCount: 0, overflowCount: 0 });
            return id;
        },
        mixerReleaseChannel: (id) => channels.delete(id),
        mixerPushChannel: (id, data, _rate) => {
            const c = channels.get(id);
            if (!c) return false;
            c.pushedFrames += data.length / 2;
            c.fillFrames = 256;
            return true;
        },
        mixerSetChannelGain: (id, gain) => { const c = channels.get(id); if (!c) return false; c.gain = gain; return true; },
        mixerSetChannelMute: (id, mute) => { const c = channels.get(id); if (!c) return false; c.mute = mute; return true; },
        mixerSetChannelGroup: (id, group) => { const c = channels.get(id); if (!c) return false; c.group = group; return true; },
        mixerListChannels: () => [...channels.values()].map(c => ({ ...c })),
    };
}

function tmpGainsPath() {
    return path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'mixer-gains-')), 'gains.json');
}

function makeBridge(native, opts = {}) {
    const events = [];
    const bridge = initMixerBridge(() => native, {
        broadcast: (_ch, data) => events.push(data),
        gainsPath: opts.gainsPath ?? tmpGainsPath(),
        reapTickMs: opts.reapTickMs ?? 3600_000, // no reaping unless a test asks
        idleReapMs: opts.idleReapMs,
        now: opts.now,
    });
    return { bridge, events };
}

test('request/release lifecycle with events; invalid labels refused', () => {
    const native = fakeNativeMixer();
    const { bridge, events } = makeBridge(native);
    const sender = fakeSender(1);

    assert.deepEqual(bridge.requestChannel(sender, '<script>', 'stems'), { refused: 'invalid-label' });

    const res = bridge.requestChannel(sender, 'stems', 'stems');
    assert.equal(res.channelId, 1);
    assert.equal(native.channels.get(1).holder, 'wc:1#stems');
    assert.equal(events.at(-1).event, 'channel-added');

    assert.equal(bridge.releaseChannel(sender, 1), true);
    assert.equal(native.channels.has(1), false);
    assert.equal(events.at(-1).event, 'channel-removed');
    bridge.dispose();
});

test('tier 3: push/release/group refused for non-holders; no handle, no writes', () => {
    const native = fakeNativeMixer();
    const { bridge } = makeBridge(native);
    const holder = fakeSender(1);
    const intruder = fakeSender(2);

    const { channelId } = bridge.requestChannel(holder, 'stems', 'stems');
    const frames = new Float32Array(512);

    assert.equal(bridge.push(intruder, channelId, frames, 48000), false);
    assert.equal(bridge.releaseChannel(intruder, channelId), false);
    assert.equal(bridge.setChannelGroup(intruder, channelId, 3), false);

    assert.equal(bridge.push(holder, channelId, frames, 48000), true);
    assert.equal(bridge.setChannelGroup(holder, channelId, 3), true);
    assert.equal(native.channels.get(channelId).group, 3);
    bridge.dispose();
});

test('tier 2 is open: gain/mute from any caller, channel-changed broadcast', () => {
    const native = fakeNativeMixer();
    const { bridge, events } = makeBridge(native);
    const holder = fakeSender(1);
    const { channelId } = bridge.requestChannel(holder, 'stems', 'stems');

    assert.equal(bridge.setChannelGain(channelId, 0.4), true);
    assert.equal(bridge.setChannelMute(channelId, true), true);
    assert.equal(native.channels.get(channelId).gain, 0.4);
    assert.equal(native.channels.get(channelId).mute, true);
    const changed = events.filter(e => e.event === 'channel-changed');
    assert.equal(changed.length, 2);
    bridge.dispose();
});

test('gain persists per holderId+label and restores on re-request (§8.8)', () => {
    const native = fakeNativeMixer();
    const gainsPath = tmpGainsPath();
    const { bridge } = makeBridge(native, { gainsPath });
    const sender = fakeSender(1);

    const { channelId } = bridge.requestChannel(sender, 'stems', 'stems');
    bridge.setChannelGain(channelId, 0.6);
    bridge.releaseChannel(sender, channelId);
    bridge.dispose();

    // Same holder+label in a fresh bridge (new session) restores 0.6.
    const { bridge: bridge2 } = makeBridge(native, { gainsPath });
    const res = bridge2.requestChannel(sender, 'stems', 'stems');
    assert.equal(native.channels.get(res.channelId).gain, 0.6);

    // A DIFFERENT holder with the same label gets the default — no bleed.
    const other = fakeSender(9);
    const res2 = bridge2.requestChannel(other, 'stems', 'other-plugin');
    assert.equal(native.channels.get(res2.channelId).gain, 1);
    bridge2.dispose();
});

test('holder death releases its channels (tier-3 lifecycle)', () => {
    const native = fakeNativeMixer();
    const { bridge, events } = makeBridge(native);
    const sender = fakeSender(1);
    const survivor = fakeSender(2);

    const a = bridge.requestChannel(sender, 'stems', 'stems').channelId;
    const b = bridge.requestChannel(sender, 'sfx', 'game').channelId;
    const c = bridge.requestChannel(survivor, 'metronome', 'metro').channelId;

    sender.emit('destroyed');
    assert.equal(native.channels.has(a), false);
    assert.equal(native.channels.has(b), false);
    assert.equal(native.channels.has(c), true);
    const removed = events.filter(e => e.event === 'channel-removed');
    assert.deepEqual(removed.map(e => e.payload.reason), ['holder-destroyed', 'holder-destroyed']);
    bridge.dispose();
});

test('idle reap releases silent unfilled channels after the window (§8.9)', async () => {
    const native = fakeNativeMixer();
    let t = 0;
    const { bridge, events } = makeBridge(native, { reapTickMs: 15, idleReapMs: 30, now: () => t });
    const sender = fakeSender(1);
    const { channelId } = bridge.requestChannel(sender, 'stems', 'stems');
    native.channels.get(channelId).fillFrames = 0;

    // Not yet past the idle window: survives the first ticks.
    t = 10;
    await new Promise(r => setTimeout(r, 25));
    assert.equal(native.channels.has(channelId), true);

    // Past the window, still silent + unfilled → reaped.
    t = 100;
    await new Promise(r => setTimeout(r, 40));
    assert.equal(native.channels.has(channelId), false);
    assert.equal(events.at(-1).payload.reason, 'idle-reaped');
    bridge.dispose();
});

test('double-audio heuristic warns once when #0 also carries audio (§8.6)', async () => {
    const native = fakeNativeMixer();
    let t = 0;
    const { bridge, events } = makeBridge(native, { reapTickMs: 10, idleReapMs: 3600_000, now: () => t });
    const sender = fakeSender(1);
    const { channelId } = bridge.requestChannel(sender, 'stems', 'stems');

    // Channel #0 live (loopback carrying audio) + bespoke channel producing.
    const zero = native.channels.get(0);
    zero.enabled = true;
    zero.fillFrames = 512;

    const warns = [];
    const original = console.warn;
    console.warn = (msg) => warns.push(String(msg));
    try {
        bridge.push(sender, channelId, new Float32Array(512), 48000);
        t += 5; await new Promise(r => setTimeout(r, 25));
        bridge.push(sender, channelId, new Float32Array(512), 48000);
        t += 5; await new Promise(r => setTimeout(r, 25));
    } finally {
        console.warn = original;
    }
    const doubleAudio = warns.filter(w => w.includes('possible double-routing'));
    assert.equal(doubleAudio.length, 1); // log-once per channel
    assert.ok(events.some(e => e.event === 'channel-diagnostic' && e.payload.kind === 'possible-double-audio'));
    bridge.dispose();
});

test('no-capacity refusal bubbles up', () => {
    const native = fakeNativeMixer();
    const { bridge } = makeBridge(native);
    const sender = fakeSender(1);
    for (let i = 1; i < 24; ++i) bridge.requestChannel(sender, `ch-${i}`, 'x');
    assert.deepEqual(bridge.requestChannel(sender, 'one-too-many', 'x'), { refused: 'no-capacity' });
    bridge.dispose();
});

test('downlevel addon (no mixer exports) fails soft', () => {
    const { bridge } = makeBridge({});
    const sender = fakeSender(1);
    assert.deepEqual(bridge.requestChannel(sender, 'stems', 'stems'), { refused: 'unavailable' });
    assert.equal(bridge.setChannelGain(1, 0.5), false);
    assert.deepEqual(bridge.listChannels(), []);
    bridge.dispose();
});
