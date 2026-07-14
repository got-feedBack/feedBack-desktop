// Lease registry (docs/audio-ownership-plan.md §2/§8): exclusive leases,
// refcounted demands, drain-then-grant takeover, reload grace window,
// user-stop suspend semantics, holder-death matrix, layered values.

const test = require('node:test');
const assert = require('node:assert/strict');
const { loadTs } = require('./_load-ts');

const { LeaseRegistry, WELL_KNOWN_HOLDERS, isValidExclusiveScope, isValidDemandScope } = loadTs('src/main/lease-registry.ts');

const WC1 = 'wc:1#nam_tone';
const WC2 = 'wc:2#rig_builder';
const CHAIN = 'signal-chain:desktop-main';

function collect(registry, event) {
    const seen = [];
    registry.on(event, e => seen.push(e));
    return seen;
}

test('scope validation', () => {
    assert.ok(isValidExclusiveScope('device-config'));
    assert.ok(isValidExclusiveScope('signal-chain:desktop-main'));
    assert.ok(isValidExclusiveScope('mixer-channel:0'));
    assert.ok(!isValidExclusiveScope('capture'));
    assert.ok(!isValidExclusiveScope('signal-chain:'));
    assert.ok(isValidDemandScope('capture'));
    assert.ok(isValidDemandScope('detection:desktop-main'));
    assert.ok(!isValidDemandScope('device-config'));
});

test('acquire / refuse / release / getHolder', () => {
    const r = new LeaseRegistry();
    const refused = collect(r, 'lease-refused');

    assert.equal(r.acquire(CHAIN, WC1).granted, true);
    assert.equal(r.getHolder(CHAIN), WC1);

    // second caller refused, holder named (refusal-by-default policy)
    const res = r.acquire(CHAIN, WC2);
    assert.deepEqual(res, { granted: false, scope: CHAIN, reason: 'held', holderId: WC1 });
    assert.equal(refused.length, 1);

    // re-acquire by the same holder is idempotent
    assert.equal(r.acquire(CHAIN, WC1).granted, true);

    // only the holder may release
    assert.equal(r.release(CHAIN, WC2), false);
    assert.equal(r.release(CHAIN, WC1), true);
    assert.equal(r.getHolder(CHAIN), null);
    assert.equal(r.acquire(CHAIN, WC2).granted, true);
    r.dispose();
});

test('invalid scopes and holders are refused, never thrown', () => {
    const r = new LeaseRegistry();
    assert.equal(r.acquire('bogus-scope', WC1).reason, 'invalid-scope');
    assert.equal(r.acquire(CHAIN, 'i-declare-myself').reason, 'invalid-holder');
    assert.equal(r.acquire(CHAIN, '').reason, 'invalid-holder');
    assert.equal(r.acquireDemand('bogus', WC1), false);
    assert.equal(r.acquireDemand('capture', 'nope nope'), false);
    // well-known internal holders pass (§8.4)
    assert.equal(r.acquire('device-config', WELL_KNOWN_HOLDERS.deviceScreen).granted, true);
    r.dispose();
});

test('takeover = drain-then-grant (§8.1)', async () => {
    const r = new LeaseRegistry();
    const revoked = collect(r, 'lease-revoked');
    r.acquire(CHAIN, WC1);

    let drained = false;
    let refusedDuringDrain = null;
    let oldCanWriteDuringDrain = null;
    const result = await r.takeover(CHAIN, WC2, async () => {
        // during the drain: old holder can no longer write, new acquires refused as 'revoking'
        oldCanWriteDuringDrain = r.canWrite(CHAIN, WC1);
        refusedDuringDrain = r.acquire(CHAIN, 'wc:3').reason;
        // revocation event fired at drain START so the old holder can stop enqueueing
        assert.equal(revoked.length, 1);
        assert.equal(revoked[0].takenOverBy, WC2);
        drained = true;
    });

    assert.equal(drained, true);
    assert.equal(oldCanWriteDuringDrain, false);
    assert.equal(refusedDuringDrain, 'revoking');
    assert.equal(result.granted, true);
    assert.equal(r.getHolder(CHAIN), WC2);
    assert.equal(r.canWrite(CHAIN, WC2), true);
    r.dispose();
});

test('takeover grants even when the drain rejects (never brick the scope)', async () => {
    const r = new LeaseRegistry();
    r.acquire(CHAIN, WC1);
    await assert.rejects(r.takeover(CHAIN, WC2, async () => { throw new Error('wedged'); }));
    // old lease is gone regardless; scope recoverable
    assert.equal(r.getHolder(CHAIN), null);
    assert.equal(r.acquire(CHAIN, WC2).granted, true);
    r.dispose();
});

test('takeover of an unheld scope is a plain acquire', async () => {
    const r = new LeaseRegistry();
    const result = await r.takeover(CHAIN, WC1);
    assert.equal(result.granted, true);
    r.dispose();
});

test('refcounted demand: idempotent per holder, active while count > 0', () => {
    const r = new LeaseRegistry();
    const changes = collect(r, 'demand-changed');

    assert.equal(r.acquireDemand('capture', WC1), true);
    assert.equal(r.acquireDemand('capture', WC1), true); // idempotent — no double count
    assert.equal(r.demandCount('capture'), 1);
    r.acquireDemand('capture', WC2);
    assert.equal(r.demandCount('capture'), 2);
    assert.equal(r.demandActive('capture'), true);

    r.releaseDemand('capture', WC1);
    assert.equal(r.demandActive('capture'), true);
    r.releaseDemand('capture', WC2);
    assert.equal(r.demandActive('capture'), false);
    // events carried count + active on every transition
    assert.deepEqual(changes.map(c => c.count), [1, 2, 1, 0]);
    r.dispose();
});

test('user stop suspends demands, only resume restores them (§8.3)', () => {
    const r = new LeaseRegistry();
    r.acquireDemand('capture', WC1);
    r.acquireDemand('detection:desktop-main', WC2);
    const suspended = collect(r, 'demand-suspended');
    const resumed = collect(r, 'demand-resumed');

    r.suspendDemands('capture');
    assert.equal(r.demandActive('capture'), false);
    assert.equal(r.demandCount('capture'), 1); // registration kept, not cleared
    assert.equal(r.demandActive('detection:desktop-main'), true); // untouched
    assert.equal(suspended.length, 1);

    // a new demand while suspended registers but does not activate the scope
    r.acquireDemand('capture', WC2);
    assert.equal(r.demandCount('capture'), 2);
    assert.equal(r.demandActive('capture'), false);

    r.resumeDemands('capture');
    assert.equal(r.demandActive('capture'), true);
    assert.equal(resumed.length, 1);
    r.dispose();
});

test('holder death releases everything immediately', () => {
    const r = new LeaseRegistry();
    const released = collect(r, 'lease-released');
    r.acquire(CHAIN, WC1);
    r.acquire('playback', WC1);
    r.acquire('device-config', WC2);
    r.acquireDemand('capture', WC1);
    r.acquireDemand('detection:desktop-main', WC1);

    r.releaseHolder(WC1);
    assert.equal(r.getHolder(CHAIN), null);
    assert.equal(r.getHolder('playback'), null);
    assert.equal(r.getHolder('device-config'), WC2); // other holders untouched
    assert.equal(r.demandCount('capture'), 0);
    assert.equal(r.demandCount('detection:desktop-main'), 0);
    assert.deepEqual(released.map(e => e.reason), ['holder-destroyed', 'holder-destroyed']);
    r.dispose();
});

test('reload grace window: same identity restores under a new holder id (§8.2)', () => {
    const r = new LeaseRegistry({ graceMs: 60_000 });
    r.acquire(CHAIN, WC1);
    r.acquireDemand('capture', WC1);

    r.beginGrace(WC1, 'plugin:nam_tone');
    // scope is free-standing during grace: getHolder empty, but restorable
    assert.equal(r.getHolder(CHAIN), null);
    assert.equal(r.demandCount('capture'), 0);

    const restored = r.tryRestore('plugin:nam_tone', 'wc:9#nam_tone');
    assert.deepEqual(restored.sort(), [CHAIN, 'capture'].sort());
    assert.equal(r.getHolder(CHAIN), 'wc:9#nam_tone');
    assert.equal(r.demandCount('capture'), 1);
    r.dispose();
});

test('grace window: a waiter that grabbed the scope meanwhile wins', () => {
    const r = new LeaseRegistry({ graceMs: 60_000 });
    r.acquire(CHAIN, WC1);
    r.beginGrace(WC1, 'plugin:nam_tone');
    r.acquire(CHAIN, WC2); // waiter takes it during the gap

    const restored = r.tryRestore('plugin:nam_tone', 'wc:9#nam_tone');
    assert.deepEqual(restored, []);
    assert.equal(r.getHolder(CHAIN), WC2);
    r.dispose();
});

test('grace window expiry releases for real', async () => {
    const r = new LeaseRegistry({ graceMs: 20 });
    const released = collect(r, 'lease-released');
    r.acquire(CHAIN, WC1);
    r.beginGrace(WC1, 'plugin:nam_tone');
    await new Promise(resolve => setTimeout(resolve, 60));
    assert.deepEqual(released.map(e => e.reason), ['grace-expired']);
    assert.deepEqual(r.tryRestore('plugin:nam_tone', 'wc:9#nam_tone'), []);
    assert.equal(r.acquire(CHAIN, WC2).granted, true);
    r.dispose();
});

test('destroy during grace cancels restore', () => {
    const r = new LeaseRegistry({ graceMs: 60_000 });
    r.acquire(CHAIN, WC1);
    r.beginGrace(WC1, 'plugin:nam_tone');
    r.releaseHolder(WC1); // renderer destroyed while grace pending
    assert.deepEqual(r.tryRestore('plugin:nam_tone', 'wc:9#nam_tone'), []);
    assert.equal(r.acquire(CHAIN, WC2).granted, true);
    r.dispose();
});

test('layered values: override rides the lease, base survives it (§2/§8.10)', () => {
    const r = new LeaseRegistry();
    const changes = collect(r, 'value-changed');

    r.setBase(CHAIN, 'noise-gate.threshold', -70);
    assert.equal(r.getEffectiveValue(CHAIN, 'noise-gate.threshold'), -70);

    // override refused without the lease
    assert.equal(r.setOverride(CHAIN, 'noise-gate.threshold', -55, WC1), false);

    r.acquire(CHAIN, WC1);
    assert.equal(r.setOverride(CHAIN, 'noise-gate.threshold', -55, WC1), true);
    assert.equal(r.getEffectiveValue(CHAIN, 'noise-gate.threshold'), -55);
    assert.equal(r.hasOverride(CHAIN, 'noise-gate.threshold'), true);

    // base write during override persists but does not pierce (§8.10)
    r.setBase(CHAIN, 'noise-gate.threshold', -80);
    assert.equal(r.getEffectiveValue(CHAIN, 'noise-gate.threshold'), -55);

    // release restores the (updated) base
    r.release(CHAIN, WC1);
    assert.equal(r.getEffectiveValue(CHAIN, 'noise-gate.threshold'), -80);
    assert.equal(r.hasOverride(CHAIN, 'noise-gate.threshold'), false);
    const last = changes[changes.length - 1];
    assert.equal(last.value, -80);
    assert.equal(last.overridden, false);
    r.dispose();
});

test('snapshot names holders with ages (diag visibility, §8.7)', () => {
    let t = 1000;
    const r = new LeaseRegistry({ now: () => t });
    r.acquire(CHAIN, WC1);
    r.acquireDemand('detection:desktop-main', WC2);
    t = 5000;
    const snap = r.snapshot();
    assert.deepEqual(snap.leases, [{ scope: CHAIN, holderId: WC1, heldMs: 4000, revoking: false, overrides: [] }]);
    assert.equal(snap.demands.length, 1);
    assert.deepEqual(snap.demands[0].holders, [{ holderId: WC2, heldMs: 4000 }]);
    r.dispose();
});
