import { EventEmitter } from 'events';

// Lease registry — ownership arbitration for audio scopes (plan: docs/audio-ownership-plan.md §2/§8).
//
// Two primitives:
//   - exclusive leases  (signal-chain:<route>, device-config, playback, ...)
//   - refcounted demands (capture, detection:<route>) — additive intent,
//     the engine acts while the holder count > 0.
//
// The registry never derives identity itself: the IPC wiring derives holder
// ids from the sender (webContents id + attributed plugin id, plan §9) and
// main-process callers use the WELL_KNOWN_HOLDERS enum (§8.4). Native stays
// dumb — per-route chainGeneration remains the tamper-evident layer below.

export type HolderId = string;

export const WELL_KNOWN_HOLDERS = Object.freeze({
    backingPlayer: 'engine:backing-player',
    startupRestore: 'main:startup-restore',
    executor: 'main:executor',
    deviceScreen: 'main:device-screen',
});

const WELL_KNOWN_SET = new Set<string>(Object.values(WELL_KNOWN_HOLDERS));

export type LeaseScope = string;

const EXCLUSIVE_SCOPE_RE = /^(device-config|playback|monitor-state|signal-chain:[\w.-]+|mixer-channel:[\w.-]+)$/;
const DEMAND_SCOPE_RE = /^(capture|detection:[\w.-]+)$/;

export type AcquireResult =
    | { granted: true; scope: LeaseScope; holderId: HolderId }
    | { granted: false; scope: LeaseScope; reason: AcquireRefusal; holderId: HolderId | null };

export type AcquireRefusal = 'held' | 'invalid-scope' | 'invalid-holder' | 'revoking';

export type ReleaseReason = 'released' | 'revoked' | 'holder-destroyed' | 'grace-expired';

type Lease = {
    holderId: HolderId;
    acquiredAt: number;
    // Set while a takeover drain is running: refuses acquires and the old
    // holder's further writes until the drain completes (§8.1).
    revoking: boolean;
    overrides: Map<string, unknown>;
};

type Demand = {
    // Idempotent per holder: one holder counts once no matter how many times
    // it asks (the five hand-rolled "did *I* start it?" hacks collapse here).
    holders: Map<HolderId, { since: number }>;
    // User raw-stop suspends demands instead of clearing them (§8.3); only a
    // user start resumes.
    suspended: boolean;
};

type GraceEntry = {
    identityKey: string;
    leases: Map<LeaseScope, Lease>;
    demands: Map<LeaseScope, { since: number }>;
    timer: NodeJS.Timeout;
    expiresAt: number;
};

export type LeaseRegistryOptions = {
    graceMs?: number;
    now?: () => number;
};

export type LeaseSnapshotEntry = {
    scope: LeaseScope;
    holderId: HolderId;
    heldMs: number;
    revoking: boolean;
    overrides: string[];
};

export type DemandSnapshotEntry = {
    scope: LeaseScope;
    suspended: boolean;
    holders: Array<{ holderId: HolderId; heldMs: number }>;
};

export function isValidExclusiveScope(scope: string): boolean {
    return EXCLUSIVE_SCOPE_RE.test(scope);
}

export function isValidDemandScope(scope: string): boolean {
    return DEMAND_SCOPE_RE.test(scope);
}

export function isWellKnownHolder(holderId: string): boolean {
    return WELL_KNOWN_SET.has(holderId);
}

function isValidHolder(holderId: unknown): holderId is HolderId {
    if (typeof holderId !== 'string' || holderId.length === 0 || holderId.length > 256) return false;
    if (WELL_KNOWN_SET.has(holderId)) return true;
    // Derived renderer identity: "wc:<webContentsId>" with optional attributed
    // plugin suffix "#<pluginId>" (soft part, plan §9).
    return /^wc:\d+(#[\w.-]{1,128})?$/.test(holderId);
}

export class LeaseRegistry extends EventEmitter {
    private leases = new Map<LeaseScope, Lease>();
    private demands = new Map<LeaseScope, Demand>();
    private baseValues = new Map<LeaseScope, Map<string, unknown>>();
    private graces = new Map<HolderId, GraceEntry>();
    private graceMs: number;
    private now: () => number;

    constructor(options: LeaseRegistryOptions = {}) {
        super();
        this.graceMs = options.graceMs ?? 8000;
        this.now = options.now ?? Date.now;
    }

    // ---- exclusive leases -------------------------------------------------

    acquire(scope: LeaseScope, holderId: HolderId): AcquireResult {
        if (!isValidExclusiveScope(scope)) {
            return this.refuse(scope, 'invalid-scope', null);
        }
        if (!isValidHolder(holderId)) {
            return this.refuse(scope, 'invalid-holder', null);
        }
        const existing = this.leases.get(scope);
        if (existing) {
            if (existing.holderId === holderId) {
                return { granted: true, scope, holderId };
            }
            return this.refuse(scope, existing.revoking ? 'revoking' : 'held', existing.holderId);
        }
        this.leases.set(scope, { holderId, acquiredAt: this.now(), revoking: false, overrides: new Map() });
        this.emit('lease-granted', { scope, holderId });
        return { granted: true, scope, holderId };
    }

    release(scope: LeaseScope, holderId: HolderId): boolean {
        const lease = this.leases.get(scope);
        if (!lease || lease.holderId !== holderId) return false;
        this.dropLease(scope, lease, 'released');
        return true;
    }

    getHolder(scope: LeaseScope): HolderId | null {
        return this.leases.get(scope)?.holderId ?? null;
    }

    // May the holder write to the scope right now? False while a takeover
    // drain is in flight (the serializer stops accepting the old holder's
    // ops the moment revocation starts, §8.1).
    canWrite(scope: LeaseScope, holderId: HolderId): boolean {
        const lease = this.leases.get(scope);
        return !!lease && lease.holderId === holderId && !lease.revoking;
    }

    // User-initiated takeover (§2 policy + §8.1 drain-then-grant). `drain`
    // is supplied by the wiring layer (executor serializer drain); the new
    // holder is granted only after it resolves.
    async takeover(scope: LeaseScope, newHolderId: HolderId, drain?: () => Promise<void>): Promise<AcquireResult> {
        if (!isValidExclusiveScope(scope)) return this.refuse(scope, 'invalid-scope', null);
        if (!isValidHolder(newHolderId)) return this.refuse(scope, 'invalid-holder', null);
        const lease = this.leases.get(scope);
        if (!lease) return this.acquire(scope, newHolderId);
        if (lease.holderId === newHolderId) return { granted: true, scope, holderId: newHolderId };
        if (lease.revoking) return this.refuse(scope, 'revoking', lease.holderId);

        lease.revoking = true;
        this.emit('lease-revoked', { scope, holderId: lease.holderId, takenOverBy: newHolderId });
        try {
            if (drain) await drain();
        } finally {
            // The old lease dies even if the drain failed — a wedged drain
            // must not brick the scope (§2: never brick until restart).
            this.dropLease(scope, lease, 'revoked');
        }
        return this.acquire(scope, newHolderId);
    }

    // ---- refcounted demands ------------------------------------------------

    acquireDemand(scope: LeaseScope, holderId: HolderId): boolean {
        if (!isValidDemandScope(scope) || !isValidHolder(holderId)) return false;
        let demand = this.demands.get(scope);
        if (!demand) {
            demand = { holders: new Map(), suspended: false };
            this.demands.set(scope, demand);
        }
        if (!demand.holders.has(holderId)) {
            demand.holders.set(holderId, { since: this.now() });
            this.emitDemandChanged(scope, demand);
        }
        return true;
    }

    releaseDemand(scope: LeaseScope, holderId: HolderId): boolean {
        const demand = this.demands.get(scope);
        if (!demand || !demand.holders.delete(holderId)) return false;
        this.emitDemandChanged(scope, demand);
        return true;
    }

    demandCount(scope: LeaseScope): number {
        return this.demands.get(scope)?.holders.size ?? 0;
    }

    // Effective demand: what the engine should act on. Zero while suspended.
    demandActive(scope: LeaseScope): boolean {
        const demand = this.demands.get(scope);
        return !!demand && !demand.suspended && demand.holders.size > 0;
    }

    // User raw-stop (§8.3): suspend every demand scope matching the prefix
    // ('capture', or '' for all). Holders keep their registration.
    suspendDemands(scopePrefix = ''): void {
        for (const [scope, demand] of this.demands) {
            if (!scope.startsWith(scopePrefix) || demand.suspended) continue;
            demand.suspended = true;
            this.emit('demand-suspended', { scope });
            this.emitDemandChanged(scope, demand);
        }
    }

    resumeDemands(scopePrefix = ''): void {
        for (const [scope, demand] of this.demands) {
            if (!scope.startsWith(scopePrefix) || !demand.suspended) continue;
            demand.suspended = false;
            this.emit('demand-resumed', { scope });
            this.emitDemandChanged(scope, demand);
        }
    }

    // ---- layered values (base + override, §2) -------------------------------

    setBase(scope: LeaseScope, key: string, value: unknown): void {
        let map = this.baseValues.get(scope);
        if (!map) {
            map = new Map();
            this.baseValues.set(scope, map);
        }
        map.set(key, value);
        // Base writes are always accepted, even during an override (§8.10) —
        // they persist and re-apply on release.
        this.emitValueChanged(scope, key);
    }

    setOverride(scope: LeaseScope, key: string, value: unknown, holderId: HolderId): boolean {
        const lease = this.leases.get(scope);
        if (!lease || lease.holderId !== holderId || lease.revoking) return false;
        lease.overrides.set(key, value);
        this.emitValueChanged(scope, key);
        return true;
    }

    getEffectiveValue(scope: LeaseScope, key: string): unknown {
        const lease = this.leases.get(scope);
        if (lease && lease.overrides.has(key)) return lease.overrides.get(key);
        return this.baseValues.get(scope)?.get(key);
    }

    hasOverride(scope: LeaseScope, key: string): boolean {
        return this.leases.get(scope)?.overrides.has(key) ?? false;
    }

    // ---- holder lifecycle ----------------------------------------------------

    // webContents destroyed / plugin teardown: everything goes, immediately.
    releaseHolder(holderId: HolderId): void {
        this.cancelGrace(holderId);
        for (const [scope, lease] of this.leases) {
            if (lease.holderId === holderId) this.dropLease(scope, lease, 'holder-destroyed');
        }
        for (const [scope, demand] of this.demands) {
            if (demand.holders.delete(holderId)) this.emitDemandChanged(scope, demand);
        }
    }

    // webContents reload (§8.2): leases + demands enter a grace window keyed
    // on manifest identity. The same identity re-requesting within the window
    // restores them (possibly under a new webContents-derived holder id);
    // expiry releases for real.
    beginGrace(holderId: HolderId, identityKey: string): void {
        this.cancelGrace(holderId);
        const heldLeases = new Map<LeaseScope, Lease>();
        for (const [scope, lease] of this.leases) {
            if (lease.holderId === holderId) {
                heldLeases.set(scope, lease);
                this.leases.delete(scope);
                this.emit('lease-suspended', { scope, holderId, identityKey });
            }
        }
        const heldDemands = new Map<LeaseScope, { since: number }>();
        for (const [scope, demand] of this.demands) {
            const entry = demand.holders.get(holderId);
            if (entry) {
                heldDemands.set(scope, entry);
                demand.holders.delete(holderId);
                this.emitDemandChanged(scope, demand);
            }
        }
        if (heldLeases.size === 0 && heldDemands.size === 0) return;
        const timer = setTimeout(() => this.expireGrace(holderId), this.graceMs);
        timer.unref?.();
        this.graces.set(holderId, {
            identityKey,
            leases: heldLeases,
            demands: heldDemands,
            timer,
            expiresAt: this.now() + this.graceMs,
        });
    }

    // Reloaded identity comes back (same manifest identity, fresh holder id).
    // Returns the scopes restored.
    tryRestore(identityKey: string, newHolderId: HolderId): LeaseScope[] {
        if (!isValidHolder(newHolderId)) return [];
        const restored: LeaseScope[] = [];
        for (const [oldHolderId, grace] of this.graces) {
            if (grace.identityKey !== identityKey) continue;
            clearTimeout(grace.timer);
            this.graces.delete(oldHolderId);
            for (const [scope, lease] of grace.leases) {
                if (this.leases.has(scope)) continue; // grabbed meanwhile — refuse quietly, waiter won
                lease.holderId = newHolderId;
                lease.revoking = false;
                this.leases.set(scope, lease);
                restored.push(scope);
                this.emit('lease-granted', { scope, holderId: newHolderId, restored: true });
            }
            for (const [scope, entry] of grace.demands) {
                let demand = this.demands.get(scope);
                if (!demand) {
                    demand = { holders: new Map(), suspended: false };
                    this.demands.set(scope, demand);
                }
                if (!demand.holders.has(newHolderId)) {
                    demand.holders.set(newHolderId, entry);
                    restored.push(scope);
                    this.emitDemandChanged(scope, demand);
                }
            }
        }
        return restored;
    }

    // ---- diagnostics ----------------------------------------------------------

    snapshot(): { leases: LeaseSnapshotEntry[]; demands: DemandSnapshotEntry[]; graces: Array<{ identityKey: string; expiresInMs: number }> } {
        const now = this.now();
        return {
            leases: [...this.leases.entries()].map(([scope, lease]) => ({
                scope,
                holderId: lease.holderId,
                heldMs: now - lease.acquiredAt,
                revoking: lease.revoking,
                overrides: [...lease.overrides.keys()],
            })),
            demands: [...this.demands.entries()]
                .filter(([, demand]) => demand.holders.size > 0 || demand.suspended)
                .map(([scope, demand]) => ({
                    scope,
                    suspended: demand.suspended,
                    holders: [...demand.holders.entries()].map(([holderId, entry]) => ({
                        holderId,
                        heldMs: now - entry.since,
                    })),
                })),
            graces: [...this.graces.values()].map(grace => ({
                identityKey: grace.identityKey,
                expiresInMs: Math.max(0, grace.expiresAt - now),
            })),
        };
    }

    dispose(): void {
        for (const grace of this.graces.values()) clearTimeout(grace.timer);
        this.graces.clear();
        this.removeAllListeners();
    }

    // ---- internals --------------------------------------------------------------

    private refuse(scope: LeaseScope, reason: AcquireRefusal, holderId: HolderId | null): AcquireResult {
        this.emit('lease-refused', { scope, reason, holderId });
        return { granted: false, scope, reason, holderId };
    }

    private dropLease(scope: LeaseScope, lease: Lease, reason: ReleaseReason): void {
        this.leases.delete(scope);
        const clearedOverrides = [...lease.overrides.keys()];
        lease.overrides.clear();
        this.emit('lease-released', { scope, holderId: lease.holderId, reason });
        // Overrides die with the lease; the base value is effective again.
        for (const key of clearedOverrides) this.emitValueChanged(scope, key);
    }

    private expireGrace(holderId: HolderId): void {
        const grace = this.graces.get(holderId);
        if (!grace) return;
        this.graces.delete(holderId);
        for (const [scope, lease] of grace.leases) {
            this.emit('lease-released', { scope, holderId: lease.holderId, reason: 'grace-expired' });
            for (const key of lease.overrides.keys()) this.emitValueChanged(scope, key);
        }
        // Demands parked in the grace were already removed from the counts.
    }

    private cancelGrace(holderId: HolderId): void {
        const grace = this.graces.get(holderId);
        if (!grace) return;
        clearTimeout(grace.timer);
        this.graces.delete(holderId);
        for (const [scope, lease] of grace.leases) {
            this.emit('lease-released', { scope, holderId: lease.holderId, reason: 'holder-destroyed' });
        }
    }

    private emitDemandChanged(scope: LeaseScope, demand: Demand): void {
        this.emit('demand-changed', {
            scope,
            count: demand.holders.size,
            active: !demand.suspended && demand.holders.size > 0,
            suspended: demand.suspended,
        });
    }

    private emitValueChanged(scope: LeaseScope, key: string): void {
        this.emit('value-changed', { scope, key, value: this.getEffectiveValue(scope, key), overridden: this.hasOverride(scope, key) });
    }
}
