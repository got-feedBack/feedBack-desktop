// Lease bridge — wires the LeaseRegistry (docs/audio-ownership-plan.md §2/§8)
// to Electron IPC. Owns the three things the registry deliberately does not:
//
//   1. holder-identity DERIVATION from the IPC sender (webContents id + an
//      optional caller-attributed tag — the compound identity of §9: the
//      webContents part is enforced, the tag part is soft attribution),
//   2. webContents lifecycle → death invalidation / reload grace windows,
//   3. the demand→engine glue: capture demand drives startAudio/stopAudio,
//      detection demand drives setNoteDetectionEnabled.
//
// The ipcMain.handle registrations live in audio-bridge.ts (thin wrappers over
// this module) so the contract snapshot keeps seeing every channel in one file.

import type { WebContents } from 'electron';
import { LeaseRegistry, HolderId } from './lease-registry';

type AudioModule = Record<string, (...args: any[]) => any> | null;

type BroadcastFn = (channel: string, data: unknown) => void;

// Default broadcast: every open BrowserWindow. Lazily required so the module
// stays loadable in the node:test harness (no electron runtime there).
function electronBroadcast(channel: string, data: unknown): void {
    // eslint-disable-next-line @typescript-eslint/no-var-requires
    const { BrowserWindow } = require('electron');
    for (const win of BrowserWindow.getAllWindows()) {
        if (!win.isDestroyed()) win.webContents.send(channel, data);
    }
}

const TAG_RE = /^[\w.-]{1,128}$/;
const DETECTION_SCOPE_DEFAULT = 'detection:desktop-main';

export type LeaseBridge = {
    registry: LeaseRegistry;
    acquire(sender: WebContents, scope: unknown, tag: unknown): unknown;
    release(sender: WebContents, scope: unknown, tag: unknown): boolean;
    takeover(sender: WebContents, scope: unknown, tag: unknown): Promise<unknown>;
    getHolder(scope: unknown): HolderId | null;
    acquireDemand(sender: WebContents, scope: unknown, tag: unknown): boolean;
    releaseDemand(sender: WebContents, scope: unknown, tag: unknown): boolean;
    snapshot(): unknown;
    // Hooks for the legacy raw channels (§6.8 migration semantics):
    onUserStartAudio(): void;
    onUserStopAudio(): void;
    // §8.3 user-stop latch: while set, legacy raw startAudio (nam_tone's
    // keep-alive watchdog, any unmigrated caller) must be suppressed — only
    // an explicit user start clears it.
    isUserStopLatched(): boolean;
    // true = swallow the raw disarm because a demand holder still needs
    // detection armed (the 6.3 "last disarmer kills a concurrent consumer" fix).
    shouldIgnoreRawDetectionDisarm(): boolean;
    noteLegacyCall(sender: WebContents, surface: string): void;
    dispose(): void;
};

export function initLeaseBridge(getAudio: () => AudioModule, options: { broadcast?: BroadcastFn } = {}): LeaseBridge {
    const registry = new LeaseRegistry();
    const broadcastFn = options.broadcast ?? electronBroadcast;

    // Holder ids this bridge minted, per webContents id — the set we must
    // invalidate when that webContents dies or reloads.
    const mintedBySender = new Map<number, Set<HolderId>>();
    const watchedSenders = new Set<number>();
    // Log-once-per-session-per-caller telemetry for legacy surfaces (§6.8:
    // "that telemetry IS the migration progress dashboard").
    const legacyCallsLogged = new Set<string>();
    // Whether the engine is running because capture demand started it (as
    // opposed to a user start). Demand-started engines stop when the demand
    // drains; user-started engines only stop on user stop (§8.3).
    let engineStartedByDemand = false;
    // User-stop latch (§8.3): the device screen's explicit stop wins over
    // EVERYTHING — legacy raw starts and fresh capture demands are held off
    // until the user starts again. Found in the field: nam_tone's 1.5 s
    // keep-alive watchdog restarted the engine right after a user stop.
    let userStopLatch = false;

    function sanitizeTag(tag: unknown): string | null {
        if (typeof tag !== 'string' || !TAG_RE.test(tag)) return null;
        return tag;
    }

    function deriveHolder(sender: WebContents, tag: unknown): HolderId {
        const cleanTag = sanitizeTag(tag);
        return cleanTag ? `wc:${sender.id}#${cleanTag}` : `wc:${sender.id}`;
    }

    // Grace identity (§8.2): the webContents id survives a reload, so both
    // tagged and untagged holders restore under the same key.
    function identityKey(sender: WebContents, tag: unknown): string {
        const cleanTag = sanitizeTag(tag);
        return cleanTag ? `wc:${sender.id}#${cleanTag}` : `wc:${sender.id}`;
    }

    function trackSender(sender: WebContents, holderId: HolderId): void {
        let minted = mintedBySender.get(sender.id);
        if (!minted) {
            minted = new Set();
            mintedBySender.set(sender.id, minted);
        }
        minted.add(holderId);
        if (watchedSenders.has(sender.id)) return;
        watchedSenders.add(sender.id);

        sender.once('destroyed', () => {
            const ids = mintedBySender.get(sender.id);
            mintedBySender.delete(sender.id);
            watchedSenders.delete(sender.id);
            if (ids) for (const id of ids) registry.releaseHolder(id);
        });
        // A main-frame navigation (reload or page swap) drops the renderer
        // context: everything it held goes into the grace window; the same
        // identity re-acquiring restores it (§8.2).
        sender.on('did-start-navigation', (_event, _url, _isInPlace, isMainFrame) => {
            if (!isMainFrame) return;
            const ids = mintedBySender.get(sender.id);
            if (!ids) return;
            for (const id of ids) registry.beginGrace(id, id);
            ids.clear();
        });
    }

    function broadcast(event: string, payload: unknown): void {
        try {
            broadcastFn('audio:leases:event', { event, payload });
        } catch (e) {
            console.warn(`[leases] event broadcast failed: ${e instanceof Error ? e.message : String(e)}`);
        }
    }

    for (const event of [
        'lease-granted', 'lease-released', 'lease-revoked', 'lease-refused', 'lease-suspended',
        'demand-changed', 'demand-suspended', 'demand-resumed', 'value-changed',
    ]) {
        registry.on(event, payload => broadcast(event, payload));
    }

    // ── demand → engine glue ────────────────────────────────────────────────

    registry.on('demand-changed', ({ scope, active }: { scope: string; active: boolean }) => {
        const audio = getAudio();
        if (!audio) return;
        if (scope === 'capture') {
            try {
                // A demand arriving while the user has stopped audio does not
                // restart the engine — it waits for the user start (§8.3).
                if (userStopLatch && active) return;
                const running = typeof audio.isAudioRunning === 'function' && audio.isAudioRunning() === true;
                if (active && !running) {
                    audio.startAudio?.();
                    engineStartedByDemand = true;
                } else if (!active && running && engineStartedByDemand) {
                    // Only stop an engine the demand path started; a
                    // user-started engine outlives its demands (§8.3).
                    audio.stopAudio?.();
                    engineStartedByDemand = false;
                }
            } catch (e) {
                console.warn(`[leases] capture demand glue failed: ${e instanceof Error ? e.message : String(e)}`);
            }
        } else if (scope.startsWith('detection:')) {
            // v1: the native pipeline has one global detection gate; any
            // active detection scope arms it. Per-route arming follows the
            // per-route native split (plan phase C/E).
            try {
                if (typeof audio.setNoteDetectionEnabled === 'function') {
                    const anyActive = registry.demandActive(scope) || anyDetectionActive();
                    audio.setNoteDetectionEnabled(anyActive);
                }
            } catch (e) {
                console.warn(`[leases] detection demand glue failed: ${e instanceof Error ? e.message : String(e)}`);
            }
        }
    });

    function anyDetectionActive(): boolean {
        return registry.snapshot().demands.some(d => d.scope.startsWith('detection:') && !d.suspended && d.holders.length > 0);
    }

    return {
        registry,

        acquire(sender, scope, tag) {
            const holderId = deriveHolder(sender, tag);
            trackSender(sender, holderId);
            // A reloaded identity gets its suspended leases back first (§8.2).
            registry.tryRestore(identityKey(sender, tag), holderId);
            return registry.acquire(String(scope), holderId);
        },

        release(sender, scope, tag) {
            return registry.release(String(scope), deriveHolder(sender, tag));
        },

        async takeover(sender, scope, tag) {
            const holderId = deriveHolder(sender, tag);
            trackSender(sender, holderId);
            // Drain hook: the chain-mutation serializer joins here in phase C
            // (§8.1). Until chain ops are lease-scoped there is nothing of the
            // old holder's to drain, so the immediate grant is exact.
            return registry.takeover(String(scope), holderId);
        },

        getHolder(scope) {
            return registry.getHolder(String(scope));
        },

        acquireDemand(sender, scope, tag) {
            const holderId = deriveHolder(sender, tag);
            trackSender(sender, holderId);
            registry.tryRestore(identityKey(sender, tag), holderId);
            const ok = registry.acquireDemand(String(scope), holderId);
            // A demand born during a user stop starts suspended, so the user
            // start resumes it like every pre-existing demand (§8.3).
            if (ok && userStopLatch && String(scope).startsWith('capture')) {
                registry.suspendDemands(String(scope));
            }
            return ok;
        },

        releaseDemand(sender, scope, tag) {
            return registry.releaseDemand(String(scope), deriveHolder(sender, tag));
        },

        snapshot() {
            return registry.snapshot();
        },

        onUserStartAudio() {
            // User start is the only thing that clears the stop latch and
            // resumes suspended demands (§8.3).
            userStopLatch = false;
            engineStartedByDemand = false;
            registry.resumeDemands('capture');
            registry.resumeDemands('detection:');
        },

        onUserStopAudio() {
            // User stop always wins: the latch holds off legacy raw starts
            // and fresh demands; existing demands suspend (registration
            // kept), holders learn via demand-suspended events (§8.3).
            userStopLatch = true;
            engineStartedByDemand = false;
            registry.suspendDemands('capture');
            registry.suspendDemands('detection:');
        },

        isUserStopLatched() {
            return userStopLatch;
        },

        shouldIgnoreRawDetectionDisarm() {
            return anyDetectionActive();
        },

        noteLegacyCall(sender, surface) {
            const key = `${surface}@wc:${sender.id}`;
            if (legacyCallsLogged.has(key)) return;
            legacyCallsLogged.add(key);
            console.info(`[leases] legacy surface ${surface} called by wc:${sender.id} — migration telemetry (plan §6.8)`);
        },

        dispose() {
            registry.dispose();
            mintedBySender.clear();
            watchedSenders.clear();
        },
    };
}

export { DETECTION_SCOPE_DEFAULT };
