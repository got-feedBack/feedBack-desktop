// Single source of truth for the on-disk locations the desktop app and the
// bundled Slopsmith backend write to. The reset/repair feature (config-reset.ts)
// and its tests enumerate paths from here so the "what gets deleted" decision
// lives in exactly one place.
//
// This module is deliberately PURE — it takes a resolved `ConfigPathEnv` and
// returns categorized path lists, with no calls into electron's `app`. That
// keeps it unit-testable per-platform without an electron mock; the real wiring
// (config-reset.ts) builds the env from app.getPath(...) + python.ts's
// getConfigDir/getDLCDir/getPluginsDir.
//
// SAFETY INVARIANT (enforced by tests): the user's song library (dlcDir),
// installed plugins (pluginsDir), and ML model caches appear ONLY under
// `optInExtras` — never in the three "safe"/"full reset" categories. A reset
// must never wipe those unless the user explicitly opts in. (Constitution VI.)

import * as fs from 'fs';
import * as path from 'path';

export interface ConfigPathEnv {
    /** process.platform — selects which Electron cache dirs are relevant. */
    platform: NodeJS.Platform;
    /** Electron app.getPath('userData') — the per-OS app data root. */
    userData: string;
    /** Electron app.getPath('home'). */
    home: string;
    /** Active backend CONFIG_DIR (python.ts getConfigDir) — may be the shared
     *  ~/.local/share/slopsmith Docker dir on Linux, hence resolved, not derived. */
    configDir: string;
    /** Song library (python.ts getDLCDir) — opt-in delete only. */
    dlcDir: string;
    /** User-installed plugins dir (python.ts getPluginsDir) — opt-in delete only. */
    pluginsDir: string;
    /** Cache root for ML weights ($XDG_CACHE_HOME || ~/.cache). */
    cacheBase: string;
    /** Resolved torch cache ($TORCH_HOME || <cacheBase>/torch) — opt-in delete only. */
    torchHome: string;
    /** Resolved HF cache ($HF_HOME || <cacheBase>/huggingface) — opt-in delete only. */
    hfHome: string;
}

export interface ConfigPathCategories {
    /** Desktop prefs + Electron caches. Safe default reset — all re-created or
     *  re-downloaded on next launch; loses device/soundfont/UI prefs only. */
    appSettingsAndCaches: string[];
    /** Plugin enable/disable state + plugin-installed Python deps + plugin data.
     *  Fixes most "stale after upgrade" symptoms; does NOT remove installed plugins. */
    pluginStateAndPyDeps: string[];
    /** Backend SQLite DBs + config.json + cached content dirs under CONFIG_DIR.
     *  Part of a full reset; never includes the song library. */
    configDbsAndState: string[];
    /** Explicit opt-ins, each off by default and each with its own warning. */
    optInExtras: {
        installedPlugins: string[];
        songLibrary: string[];
        mlCaches: string[];
    };
}

// SQLite DBs written under CONFIG_DIR by the backend / bundled plugins.
const CONFIG_DB_FILES = [
    'web_library.db',
    'audio_effects.db',
    'nam_tone.db',
    'studio.db',
    'practice_journal.db',
    'midi_mappings.db',
    'rig_builder_cache.db',
];

// Cached/generated content dirs under CONFIG_DIR (not the song library).
const CONFIG_STATE_DIRS = ['tutorials', 'minigames', 'achievements', 'sloppak_cache'];

// Chromium/Electron state dirs under userData. Deleting these resets origin-keyed
// UI settings, GPU shader cache, HTTP cache, etc. — all rebuilt on next launch.
const ELECTRON_STATE = ['Preferences', 'Local Storage', 'Cache', 'GPUCache', 'Code Cache'];

// Paths a live process holds open: Chromium rewrites these on quit and Windows
// blocks deleting open files, so deleting them while the app runs is unreliable.
// They are DEFERRED to the next launch and applied before any BrowserWindow /
// crashReporter is created (config-bootstrap.consumePendingReset). 'Crashpad' is
// included because crashReporter.start() reopens it at top of main.
export const DEFERRED_BASENAMES = [...ELECTRON_STATE, 'Crashpad'];

// Expand a base SQLite filename to itself plus its WAL/SHM sidecars — an abrupt
// (SIGKILL) backend stop in WAL mode leaves *.db-wal / *.db-shm beside *.db, and
// a reset must clear those too or stale committed-but-uncheckpointed state lingers.
function withSqliteSidecars(dbFiles: string[]): string[] {
    return dbFiles.flatMap((f) => [f, `${f}-wal`, `${f}-shm`]);
}

export function enumerateConfigPaths(env: ConfigPathEnv): ConfigPathCategories {
    const u = env.userData;
    const c = env.configDir;

    const appSettingsAndCaches = [
        // Desktop prefs (soundfont quality, LAN access) — soundfont-manager.ts
        path.join(u, 'slopsmith-desktop.json'),
        // Audio device settings — audio-bridge.ts
        path.join(u, 'slopsmith-audio-settings.json'),
        // Downloaded high-quality soundfont cache — soundfont-manager.ts
        path.join(u, 'soundfonts'),
        // VST crash-guard sentinel + blocklist — vst-crash-guard.ts
        path.join(u, 'vst-load-sentinel.json'),
        path.join(u, 'vst-crash-blocklist.json'),
        // Native VST plugin registry cache — audio-bridge.ts
        path.join(u, 'known-plugins.xml'),
        // Native crash dumps — main.ts crashReporter
        path.join(u, 'Crashpad'),
        ...ELECTRON_STATE.map((d) => path.join(u, d)),
    ];

    const pluginStateAndPyDeps = [
        path.join(c, 'plugin_state.json'),
        path.join(c, 'pip_packages'),
        path.join(c, 'plugin_data'),
    ];

    const configDbsAndState = [
        ...withSqliteSidecars(CONFIG_DB_FILES).map((f) => path.join(c, f)),
        path.join(c, 'config.json'),
        // The migration stamp must go too — otherwise a full reset leaves it at
        // the latest schema and the next startup skips migrations that would
        // recreate/repair the freshly-reset config (config-migrations.ts).
        path.join(c, 'config_version.json'),
        ...CONFIG_STATE_DIRS.map((d) => path.join(c, d)),
    ];

    const optInExtras = {
        installedPlugins: [env.pluginsDir],
        songLibrary: [env.dlcDir],
        // Resolved from the env so a custom TORCH_HOME / HF_HOME is honored — the
        // app sets those for the backend (python.ts startPython), so the reset
        // must target the same locations, not just the cacheBase defaults.
        mlCaches: [env.torchHome, env.hfHome],
    };

    return { appSettingsAndCaches, pluginStateAndPyDeps, configDbsAndState, optInExtras };
}

// ── Reset selection → delete set (pure, testable without electron) ────────────

export interface ResetSelection {
    /** Desktop prefs + Electron caches (safe default). */
    appSettings?: boolean;
    /** plugin_state.json + pip_packages + plugin_data. */
    pluginState?: boolean;
    /** Everything above + backend DBs/config under CONFIG_DIR. */
    fullReset?: boolean;
    /** Opt-in: also remove the user-installed plugins dir. */
    alsoInstalledPlugins?: boolean;
    /** Opt-in: also delete the song library (DLC_DIR). */
    alsoSongLibrary?: boolean;
    /** Opt-in: also clear ML model caches (torch / huggingface). */
    alsoMlCaches?: boolean;
}

export interface ResetEntry {
    path: string;
    existed: boolean;
    ok: boolean;
    error?: string;
    /** True when deletion was deferred to next launch (Chromium-held path). */
    deferred?: boolean;
}

/**
 * Assemble the de-duplicated delete set for a selection. The opt-in extras
 * (installed plugins, song library, ML caches) are added ONLY on their explicit
 * flag — never via appSettings or fullReset. This is the function the
 * "library & plugins preserved" tests assert against.
 */
export function buildDeleteSet(selection: ResetSelection, cats: ConfigPathCategories): string[] {
    const set = new Set<string>();
    const add = (paths: string[]) => paths.forEach((p) => set.add(p));

    if (selection.appSettings || selection.fullReset) add(cats.appSettingsAndCaches);
    if (selection.pluginState || selection.fullReset) add(cats.pluginStateAndPyDeps);
    if (selection.fullReset) add(cats.configDbsAndState);

    if (selection.alsoInstalledPlugins) add(cats.optInExtras.installedPlugins);
    if (selection.alsoSongLibrary) add(cats.optInExtras.songLibrary);
    if (selection.alsoMlCaches) add(cats.optInExtras.mlCaches);

    return [...set];
}

/** Delete a list of paths fail-soft, returning a per-path summary. */
export function deletePaths(paths: string[]): ResetEntry[] {
    return paths.map((p) => {
        let existed = false;
        try {
            existed = fs.existsSync(p);
        } catch {
            /* treat unstatable as not-existing for the summary */
        }
        try {
            fs.rmSync(p, { recursive: true, force: true });
            return { path: p, existed, ok: true };
        } catch (err) {
            return { path: p, existed, ok: false, error: String(err) };
        }
    });
}

/**
 * Split a delete set into paths safe to remove immediately vs paths held open by
 * the live process (Chromium state / Crashpad), which must be deferred to the
 * next launch. Matches on basename so it's independent of the userData root.
 */
export function partitionDeferred(paths: string[]): { immediate: string[]; deferred: string[] } {
    const immediate: string[] = [];
    const deferred: string[] = [];
    for (const p of paths) {
        if (DEFERRED_BASENAMES.includes(path.basename(p))) deferred.push(p);
        else immediate.push(p);
    }
    return { immediate, deferred };
}

/** True when the active CONFIG_DIR is the Linux shared (~/.local/share/slopsmith)
 *  dir also used by a Docker Slopsmith — the UI warns before a full reset there. */
export function isSharedDockerConfig(env: ConfigPathEnv): boolean {
    const shared = path.join(env.home, '.local', 'share', 'slopsmith');
    return path.resolve(env.configDir) === path.resolve(shared);
}
