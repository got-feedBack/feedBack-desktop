// Mixer bridge — the JS half of the engine-owned mixer (plan §5.1).
// Owns what native deliberately does not:
//
//   tier 3 authority — a channel's producer handle is its requesting
//     webContents: push/release from any other sender is refused (the
//     opaque-handle rule; native only knows holder strings),
//   gain persistence — per `holderId + label` (§8.8), file-backed,
//   idle reaping — silent, unfilled channels are released after
//     kIdleReapMs (§8.9); native only enforces the hard cap,
//   double-audio heuristic — a bespoke channel active while channel #0
//     also carries audio gets a log-once diag warning (§8.6),
//   channel lifecycle events — channel-added / channel-removed /
//     channel-changed broadcast to every window (tier 1).
//
// The ipcMain.handle registrations live in audio-bridge.ts (contract
// snapshot sees every channel in one file), delegating here.

import * as fs from 'fs';
import * as path from 'path';
import type { WebContents } from 'electron';

type AudioModule = Record<string, (...args: any[]) => any> | null;
type BroadcastFn = (channel: string, data: unknown) => void;

const TAG_RE = /^[\w.-]{1,128}$/;
const LABEL_RE = /^[\w .-]{1,63}$/;
const kIdleReapMs = 5 * 60_000;
const kReapTickMs = 60_000;

function electronBroadcast(channel: string, data: unknown): void {
    // eslint-disable-next-line @typescript-eslint/no-var-requires
    const { BrowserWindow } = require('electron');
    for (const win of BrowserWindow.getAllWindows()) {
        if (!win.isDestroyed()) win.webContents.send(channel, data);
    }
}

function defaultGainsPath(): string {
    // eslint-disable-next-line @typescript-eslint/no-var-requires
    const { app } = require('electron');
    return path.join(app.getPath('userData'), 'mixer-channel-gains.json');
}

type ChannelOwnership = {
    channelId: number;
    senderId: number;
    holder: string;
    label: string;
    lastActivePushedFrames: number;
    lastActiveAt: number;
    doubleAudioWarned: boolean;
};

export type MixerBridge = {
    requestChannel(sender: WebContents, label: unknown, tag: unknown): { channelId: number } | { refused: string };
    releaseChannel(sender: WebContents, channelId: unknown): boolean;
    push(sender: WebContents, channelId: unknown, data: unknown, sourceRate: unknown): boolean;
    setChannelGain(channelId: unknown, gain: unknown): boolean;
    setChannelMute(channelId: unknown, mute: unknown): boolean;
    setChannelGroup(sender: WebContents, channelId: unknown, group: unknown): boolean;
    listChannels(): unknown[];
    dispose(): void;
};

export function initMixerBridge(
    getAudio: () => AudioModule,
    options: { broadcast?: BroadcastFn; gainsPath?: string; reapTickMs?: number; idleReapMs?: number; now?: () => number } = {},
): MixerBridge {
    const broadcast = options.broadcast ?? electronBroadcast;
    const gainsPath = options.gainsPath ?? defaultGainsPath();
    const now = options.now ?? Date.now;
    const idleReapMs = options.idleReapMs ?? kIdleReapMs;

    const owned = new Map<number, ChannelOwnership>();
    const watchedSenders = new Set<number>();

    // ── gain persistence (§8.8: keyed holderId + label, never label alone) ──

    let gains: Record<string, number> = {};
    try {
        if (fs.existsSync(gainsPath)) {
            const parsed = JSON.parse(fs.readFileSync(gainsPath, 'utf8'));
            if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
                for (const [k, v] of Object.entries(parsed)) {
                    if (typeof v === 'number' && Number.isFinite(v)) gains[k] = v;
                }
            }
        }
    } catch (e) {
        console.warn(`[mixer] gain store unreadable, starting fresh: ${e instanceof Error ? e.message : String(e)}`);
        gains = {};
    }

    function gainKey(holder: string, label: string): string {
        return `${holder}::${label}`;
    }

    function persistGains(): void {
        try {
            fs.writeFileSync(gainsPath, JSON.stringify(gains, null, 2), 'utf8');
        } catch (e) {
            console.warn(`[mixer] gain store write failed: ${e instanceof Error ? e.message : String(e)}`);
        }
    }

    function emit(event: string, payload: unknown): void {
        try { broadcast('audio:mixer:event', { event, payload }); }
        catch (e) { console.warn(`[mixer] event broadcast failed: ${e instanceof Error ? e.message : String(e)}`); }
    }

    function deriveHolder(sender: WebContents, tag: unknown): string {
        const clean = typeof tag === 'string' && TAG_RE.test(tag) ? tag : null;
        return clean ? `wc:${sender.id}#${clean}` : `wc:${sender.id}`;
    }

    function findNative(name: string): ((...args: any[]) => any) | null {
        const audio = getAudio();
        if (!audio || typeof audio[name] !== 'function') return null;
        return audio[name].bind(audio);
    }

    function releaseOwned(entry: ChannelOwnership, reason: string): void {
        owned.delete(entry.channelId);
        const release = findNative('mixerReleaseChannel');
        if (release) {
            try { release(entry.channelId); } catch { /* engine may be gone */ }
        }
        emit('channel-removed', { id: entry.channelId, label: entry.label, holder: entry.holder, reason });
    }

    function watchSender(sender: WebContents): void {
        if (watchedSenders.has(sender.id)) return;
        watchedSenders.add(sender.id);
        sender.once('destroyed', () => {
            watchedSenders.delete(sender.id);
            for (const entry of [...owned.values()]) {
                if (entry.senderId === sender.id) releaseOwned(entry, 'holder-destroyed');
            }
        });
    }

    // ── idle reap + double-audio heuristic (one shared tick) ────────────────

    const reapTimer = setInterval(() => {
        const list = findNative('mixerListChannels');
        if (!list) return;
        let channels: any[];
        try { channels = list(); } catch { return; }
        const byId = new Map<number, any>(channels.map((c: any) => [c.id, c]));
        const defaultChannel = byId.get(0);
        const t = now();

        for (const entry of [...owned.values()]) {
            const c = byId.get(entry.channelId);
            if (!c) { owned.delete(entry.channelId); continue; }

            // Activity = the producer pushed since the last tick.
            const pushed = Number(c.pushedFrames) || 0;
            const active = pushed > entry.lastActivePushedFrames;
            if (active) {
                entry.lastActivePushedFrames = pushed;
                entry.lastActiveAt = t;

                // Double-audio heuristic (§8.6): a bespoke channel carrying
                // audio while channel #0 is also enabled and filled suggests
                // the producer forgot to reroute its WebAudio graph off the
                // renderer master — it is likely playing twice. Diag hint,
                // not enforcement; log-once per channel.
                if (!entry.doubleAudioWarned && defaultChannel?.enabled && Number(defaultChannel.fillFrames) > 0) {
                    entry.doubleAudioWarned = true;
                    console.warn(`[mixer] possible double-routing: channel ${entry.channelId} (${entry.label}, ${entry.holder}) is producing while default channel #0 also carries audio (plan §8.6)`);
                    emit('channel-diagnostic', { id: entry.channelId, kind: 'possible-double-audio' });
                }
            } else if (Number(c.fillFrames) === 0 && t - entry.lastActiveAt > idleReapMs) {
                // Idle reap (§8.9): silent + unfilled past the window. The
                // holder re-requests transparently on its next push.
                releaseOwned(entry, 'idle-reaped');
            }
        }
    }, options.reapTickMs ?? kReapTickMs);
    reapTimer.unref?.();

    return {
        requestChannel(sender, label, tag) {
            const cleanLabel = typeof label === 'string' && LABEL_RE.test(label) ? label : null;
            if (!cleanLabel) return { refused: 'invalid-label' };
            const create = findNative('mixerCreateChannel');
            if (!create) return { refused: 'unavailable' };
            const holder = deriveHolder(sender, tag);
            let channelId = -1;
            try { channelId = Number(create(cleanLabel, 'plugin', holder)); } catch { channelId = -1; }
            if (!Number.isInteger(channelId) || channelId <= 0) return { refused: 'no-capacity' };

            watchSender(sender);
            owned.set(channelId, {
                channelId,
                senderId: sender.id,
                holder,
                label: cleanLabel,
                lastActivePushedFrames: 0,
                lastActiveAt: now(),
                doubleAudioWarned: false,
            });

            // Restore the persisted fader for this holder+label (§8.8).
            const savedGain = gains[gainKey(holder, cleanLabel)];
            if (typeof savedGain === 'number') {
                const setGain = findNative('mixerSetChannelGain');
                if (setGain) { try { setGain(channelId, savedGain); } catch { /* fail-soft */ } }
            }
            emit('channel-added', { id: channelId, label: cleanLabel, holder });
            return { channelId };
        },

        releaseChannel(sender, channelId) {
            const entry = owned.get(Number(channelId));
            if (!entry || entry.senderId !== sender.id) return false; // tier-3: holder only
            releaseOwned(entry, 'released');
            return true;
        },

        push(sender, channelId, data, sourceRate) {
            const entry = owned.get(Number(channelId));
            if (!entry || entry.senderId !== sender.id) return false; // no handle, no writes
            const push = findNative('mixerPushChannel');
            if (!push || !(data instanceof Float32Array)) return false;
            try { return push(entry.channelId, data, Number(sourceRate)) === true; }
            catch { return false; }
        },

        // Tier 2 — deliberately NOT ownership-gated (§5.1: the fader belongs
        // to the user; native clamps the value). Last-writer-wins is correct.
        setChannelGain(channelId, gain) {
            const setGain = findNative('mixerSetChannelGain');
            const id = Number(channelId);
            const value = Number(gain);
            if (!setGain || !Number.isInteger(id) || !Number.isFinite(value)) return false;
            let ok = false;
            try { ok = setGain(id, value) === true; } catch { return false; }
            if (ok) {
                const entry = owned.get(id);
                if (entry) {
                    gains[gainKey(entry.holder, entry.label)] = value;
                    persistGains();
                }
                emit('channel-changed', { id, gain: value });
            }
            return ok;
        },

        setChannelMute(channelId, mute) {
            const setMute = findNative('mixerSetChannelMute');
            const id = Number(channelId);
            if (!setMute || !Number.isInteger(id)) return false;
            let ok = false;
            try { ok = setMute(id, Boolean(mute)) === true; } catch { return false; }
            if (ok) emit('channel-changed', { id, mute: Boolean(mute) });
            return ok;
        },

        // Group assignment is producer-side (§8.13) — holder only.
        setChannelGroup(sender, channelId, group) {
            const entry = owned.get(Number(channelId));
            if (!entry || entry.senderId !== sender.id) return false;
            const setGroup = findNative('mixerSetChannelGroup');
            if (!setGroup) return false;
            const g = Number(group);
            try { return setGroup(entry.channelId, Number.isInteger(g) ? g : -1) === true; }
            catch { return false; }
        },

        listChannels() {
            const list = findNative('mixerListChannels');
            if (!list) return [];
            try { return list(); } catch { return []; }
        },

        dispose() {
            clearInterval(reapTimer);
            owned.clear();
            watchedSenders.clear();
        },
    };
}
