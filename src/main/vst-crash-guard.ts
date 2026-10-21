// VST crash guard — a dead-man's-pedal that learns which VST3s crash the app.
//
// Some plugins fault when loaded or when their editor is opened in-process
// (a common cause: an editor that must be created on the OS main thread,
// which the audio addon can't provide because Electron owns it). We can't
// predict which plugins are affected. So: before each risky in-process VST
// op, drop a sentinel file naming the plugin; clear it once the op has
// demonstrably survived. If a sentinel is still on disk at the next startup,
// that plugin took the app down.
//
// A single crash is not enough to condemn a plugin: an intermittent fault
// would blocklist a plugin that works most of the time, and a blocklist
// entry used to be final — there was no way to undo it from the app. So a
// plugin is only blocklisted after CRASH_BLOCK_THRESHOLD CONSECUTIVE crashes
// of the same op (load/editor), counted across app restarts in
// vst-crash-counts.json. A successful run of the same op resets that op's
// streak (an op that just worked is not "consecutive"). The blocklist itself
// is manageable from Settings → Audio (getCrashBlocklist /
// removeFromCrashBlocklist / clearCrashBlocklist); every mutation is pushed
// to the addon so routing changes apply immediately. The blocklist is handed
// to the addon, which then routes those plugins through the out-of-process
// sandbox (slopsmith-vst-host.exe).

import { app } from 'electron';
import * as fs from 'fs';
import * as path from 'path';

// A plugin is blocklisted only after this many consecutive crashes of the
// same op (load/editor) — see the header comment. High enough that an
// intermittent crasher (which usually survives) never accumulates this many
// deaths in a row; low enough that a consistently-fatal plugin is contained
// within a handful of app restarts.
export const CRASH_BLOCK_THRESHOLD = 5;

// Editor creation is asynchronous on the addon's message thread, so there is
// no synchronous success signal. Clear the sentinel after this grace window —
// if the app is still alive by then, the editor didn't fault.
const EDITOR_GRACE_MS = 6000;

let sentinelPath = '';
let blocklistPath = '';
let countsPath = '';

// Blocklist keys are normalised (lowercased on Windows) paths; the display map
// keeps the original-case path so Settings can show the user something
// recognisable. Legacy entries loaded from disk are their own display path.
const blocklist = new Set<string>();
const blocklistDisplay = new Map<string, string>();

// Per-plugin, per-op consecutive-crash streaks, persisted to
// vst-crash-counts.json across restarts.
interface OpCounts { load: number; editor: number }
const counts = new Map<string, OpCounts>();

// The plugin/op the currently-armed sentinel attributes, so a successful
// disarm can reset exactly that op's streak (see disarmSentinel).
let armedPlugin = '';
let armedOp: 'load' | 'editor' = 'load';

// Plugins that crossed the threshold at THIS startup, pending broadcast to
// the renderer (audio-bridge.ts flushes them once the renderer has painted —
// at initVstCrashGuard() time only the splash window exists, and its preload
// has no vst listener).
export interface StartupBlocked { path: string; count: number; op: 'load' | 'editor' }
const startupBlocked: StartupBlocked[] = [];

// The pending editor-grace timer, if any. Tracked so a second editor-open
// cancels the first's timer — otherwise the older timer would clear the
// newer sentinel early and a crash from the second plugin would go
// unrecorded.
let editorTimer: ReturnType<typeof setTimeout> | null = null;

// Normalise a plugin path for use as a blocklist key. Windows paths are
// case-insensitive, so fold case there to match the addon's case-insensitive
// lookup. POSIX paths are case-sensitive — lowercasing them would corrupt the
// path, so preserve case (and the lowercasing wouldn't help anyway, as the
// sandbox is Windows-only today).
const norm = (p: string): string => {
    const trimmed = p.trim();
    return process.platform === 'win32' ? trimmed.toLowerCase() : trimmed;
};

// ── Crash-count persistence ────────────────────────────────────────────────

function loadCounts(): void {
    try {
        const raw = JSON.parse(fs.readFileSync(countsPath, 'utf8'));
        if (raw && typeof raw === 'object') {
            for (const [plugin, ops] of Object.entries(raw)) {
                if (ops && typeof ops === 'object') {
                    const o = ops as Record<string, unknown>;
                    counts.set(norm(plugin), {
                        load: typeof o.load === 'number' ? o.load : 0,
                        editor: typeof o.editor === 'number' ? o.editor : 0,
                    });
                }
            }
        }
    } catch { /* no counts yet — fine */ }
}

function persistCounts(): void {
    if (!countsPath) return;
    try {
        const out: Record<string, OpCounts> = {};
        for (const [plugin, ops] of counts) out[plugin] = ops;
        fs.writeFileSync(countsPath, JSON.stringify(out, null, 2));
    } catch (e: any) {
        console.warn(`[vst-crash-guard] could not persist crash counts: ${e?.message}`);
    }
}

// Record that `plugin` crashed the app during `op` (a leftover sentinel was
// found at startup). Returns the new streak for that op.
function recordCrash(plugin: string, op: 'load' | 'editor'): number {
    const key = norm(plugin);
    let ops = counts.get(key);
    if (!ops) { ops = { load: 0, editor: 0 }; counts.set(key, ops); }
    ops[op] += 1;
    persistCounts();
    return ops[op];
}

// A successful `op` breaks that op's crash streak. The OTHER op's streak is
// left alone: a plugin that loads fine but crashes on editor-open must still
// accumulate editor crashes across sessions (each load success would
// otherwise reset the streak before the editor is ever opened).
function resetCrashCount(plugin: string, op: 'load' | 'editor'): void {
    const ops = counts.get(norm(plugin));
    if (!ops || ops[op] === 0) return;
    ops[op] = 0;
    persistCounts();
}

// ── Public management API (Settings → Audio) ───────────────────────────────

// Display paths (original case) of every blocklisted plugin.
export function getCrashBlocklist(): string[] {
    return [...blocklistDisplay.values()];
}

// Remove one entry (matched case-insensitively on Windows). Also drops the
// plugin's crash streaks, so a re-block needs a fresh run of consecutive
// crashes. Returns whether the entry was present.
export function removeFromCrashBlocklist(pluginPath: string): boolean {
    const key = norm(pluginPath);
    if (!blocklist.delete(key)) return false;
    const display = blocklistDisplay.get(key) ?? key;
    blocklistDisplay.delete(key);
    counts.delete(key);
    persistCounts();
    persist();
    console.log(`[vst-crash-guard] ${display} removed from the crash blocklist`);
    return true;
}

// Drop every entry and every crash streak. Returns how many entries were
// removed.
export function clearCrashBlocklist(): number {
    const removed = blocklist.size;
    if (removed) console.log(`[vst-crash-guard] crash blocklist cleared (${removed} ${removed === 1 ? 'entry' : 'entries'})`);
    blocklist.clear();
    blocklistDisplay.clear();
    counts.clear();
    persistCounts();
    persist();
    return removed;
}

// Startup promotions not yet broadcast to the renderer (consumed by
// audio-bridge.ts flushVstBlockedNotifications).
export function consumeStartupBlocked(): StartupBlocked[] {
    return startupBlocked.splice(0, startupBlocked.length);
}

// Run once at startup, before any VST can be loaded. Promotes a leftover
// sentinel (= the app crashed mid-op last run) into the crash streak, and
// into the persistent blocklist once the streak reaches CRASH_BLOCK_THRESHOLD,
// then returns the full blocklist for handing to the addon.
export function initVstCrashGuard(): string[] {
    const dir = app.getPath('userData');
    sentinelPath = path.join(dir, 'vst-load-sentinel.json');
    blocklistPath = path.join(dir, 'vst-crash-blocklist.json');
    countsPath = path.join(dir, 'vst-crash-counts.json');

    try {
        const raw = JSON.parse(fs.readFileSync(blocklistPath, 'utf8'));
        if (Array.isArray(raw))
            for (const p of raw)
                if (typeof p === 'string' && p) {
                    const key = norm(p);
                    blocklist.add(key);
                    if (!blocklistDisplay.has(key)) blocklistDisplay.set(key, p);
                }
    } catch { /* no blocklist yet — fine */ }

    loadCounts();

    try {
        const s = JSON.parse(fs.readFileSync(sentinelPath, 'utf8'));
        if (s && typeof s.plugin === 'string' && s.plugin) {
            const op: 'load' | 'editor' = s.op === 'editor' ? 'editor' : 'load';
            const key = norm(s.plugin);
            if (!blocklist.has(key)) {
                const streak = recordCrash(s.plugin, op);
                if (streak >= CRASH_BLOCK_THRESHOLD) {
                    blocklist.add(key);
                    blocklistDisplay.set(key, s.plugin);
                    persist();
                    console.warn(`[vst-crash-guard] ${s.plugin} crashed the app during `
                        + `'${op}' for the ${streak}th consecutive time — blocklisted; `
                        + `it will load sandboxed from now on`);
                    startupBlocked.push({ path: s.plugin, count: streak, op });
                } else {
                    console.warn(`[vst-crash-guard] ${s.plugin} crashed the app during `
                        + `'${op}' (crash ${streak}/${CRASH_BLOCK_THRESHOLD} — not blocklisted yet)`);
                }
            }
        }
    } catch { /* no sentinel — the app exited cleanly last run */ }
    clearSentinel();

    // A clean shutdown is, by definition, not a crash. If the app quits while
    // an editor grace-window sentinel is still armed (the unref'd timer
    // hasn't fired yet), disarm it here so the plugin isn't falsely
    // blocklisted on next launch — only an abrupt termination should leave a
    // sentinel. disarmSentinel also cancels the pending timer so no stale
    // callback can fire mid-shutdown. The streak is NOT reset here: a clean
    // quit is not a successful op (the grace window never elapsed), so a
    // crash-prone plugin keeps its count.
    app.once('will-quit', () => disarmSentinel(false));

    return [...blocklist];
}

// Absolute path to the sentinel file, for handing to the native crash
// attributor (NodeAddon setVstCrashSentinelPath) so a fatal in-process VST3
// fault can stamp the same sentinel this module promotes on next launch. Empty
// until initVstCrashGuard() has run.
export function getSentinelPath(): string {
    return sentinelPath;
}

function persist(): void {
    try {
        fs.writeFileSync(blocklistPath, JSON.stringify([...blocklistDisplay.values()], null, 2));
    } catch (e: any) {
        console.warn(`[vst-crash-guard] could not persist blocklist: ${e?.message}`);
    }
}

function clearSentinel(): void {
    try {
        if (sentinelPath) fs.rmSync(sentinelPath, { force: true });
    } catch { /* best-effort */ }
}

// Drop the sentinel just before a risky in-process VST op. A plain
// writeFileSync is enough: an access violation kills the process but not the
// OS page cache, so the file survives for the next startup to find.
//
// Cancels any pending editor-grace timer first: that timer would otherwise
// clear this fresh sentinel mid-op (e.g. an editor-open followed by a load
// inside the grace window), losing the crash attribution.
export function armSentinel(pluginPath: string, op: 'load' | 'editor'): void {
    if (!pluginPath || !sentinelPath) return;
    if (editorTimer) { clearTimeout(editorTimer); editorTimer = null; }
    armedPlugin = norm(pluginPath);
    armedOp = op;
    try {
        fs.writeFileSync(sentinelPath,
            JSON.stringify({ plugin: pluginPath, op, at: Date.now() }));
    } catch { /* best-effort — a missed sentinel just means no auto-blocklist */ }
}

// Clear the sentinel once the armed op has survived. A survival breaks that
// op's crash streak (resetCount=true, the default — used after a synchronous
// load and when an editor-open returns without creating a window). Pass false
// for a neutral clear that leaves the streak alone (app quit mid-grace: not a
// crash, but not a demonstrated success either).
// Also cancels a pending editor-grace timer so it can't fire stale later.
export function disarmSentinel(resetCount = true): void {
    if (editorTimer) { clearTimeout(editorTimer); editorTimer = null; }
    if (resetCount && armedPlugin) resetCrashCount(armedPlugin, armedOp);
    armedPlugin = '';
    clearSentinel();
}

// Arm for an editor-open and self-clear after the grace window. armSentinel
// cancels any pending grace timer from an earlier editor-open, so an older
// timer can't clear this newer sentinel early. The timer is unref'd so it
// never holds the app open on its own.
export function armEditorSentinel(pluginPath: string): void {
    if (!pluginPath || !sentinelPath) return;
    armSentinel(pluginPath, 'editor');
    editorTimer = setTimeout(() => {
        editorTimer = null;
        disarmSentinel();
    }, EDITOR_GRACE_MS);
    editorTimer.unref();
}
