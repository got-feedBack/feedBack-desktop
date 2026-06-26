// In-app "Reset / repair configuration" — the replacement for telling testers to
// manually delete the config folder. Enumerates the correct paths for the
// current OS (config-paths.ts), stops the Python backend so it releases DB/file
// handles, deletes the selected categories, and reports a per-path summary.
//
// SAFETY (Constitution VI): the song library and installed plugins live under
// optInExtras and are deleted ONLY when the matching flag is set. The three
// non-opt-in categories never include them.

import * as path from 'path';
import { app, ipcMain, dialog, BrowserWindow } from 'electron';
import { stopPython, getConfigDir, getDLCDir, getPluginsDir } from './python';
import {
    enumerateConfigPaths,
    buildDeleteSet,
    deletePaths,
    partitionDeferred,
    isSharedDockerConfig,
    ConfigPathEnv,
    ResetSelection,
    ResetEntry,
} from './config-paths';
import { schedulePendingDeletion } from './config-bootstrap';
import * as fs from 'fs';
import {
    IPC_MAINTENANCE_GET_PATHS,
    IPC_MAINTENANCE_RESET,
    IPC_MAINTENANCE_RESTART,
} from './ipc-channels';

// Re-export so the preload bridge (and other consumers) can import the reset
// types from this module's public surface.
export type { ResetSelection, ResetEntry } from './config-paths';

export interface ResetSummary {
    selection: ResetSelection;
    configDir: string;
    deleted: ResetEntry[];
    /** True when the user dismissed the native confirmation; nothing was deleted. */
    canceled?: boolean;
}

// Human-readable confirmation copy for the native dialog. Highlights the
// destructive opt-ins explicitly so the gate can't be glossed over.
function describeSelection(s: ResetSelection): { message: string; detail: string } {
    const lines: string[] = [];
    if (s.fullReset) {
        lines.push('Delete the configuration and databases (settings, library index, tones, plugin state).');
    } else {
        if (s.appSettings) lines.push('Reset app settings & caches.');
        if (s.pluginState) lines.push('Clear plugin state & cached Python deps.');
    }
    const extras: string[] = [];
    if (s.alsoInstalledPlugins) extras.push('installed plugins');
    if (s.alsoSongLibrary) extras.push('your song library');
    if (s.alsoMlCaches) extras.push('ML model caches');
    if (extras.length) lines.push(`ALSO permanently delete: ${extras.join(', ')}.`);
    return {
        message: 'Reset / repair configuration?',
        detail: `${lines.join('\n')}\n\nThis cannot be undone. The app will restart afterwards.`,
    };
}

// Cache root for ML weights — mirrors python.ts startPython's cacheBase.
function cacheBase(): string {
    return process.env.XDG_CACHE_HOME || path.join(app.getPath('home'), '.cache');
}

/** Build the resolved path env from electron + python.ts getters. The ML cache
 *  paths mirror startPython()'s resolution exactly (TORCH_HOME / HF_HOME override
 *  the cacheBase defaults) so a reset targets the directories actually in use. */
export function buildConfigPathEnv(): ConfigPathEnv {
    const cb = cacheBase();
    return {
        platform: process.platform,
        userData: app.getPath('userData'),
        home: app.getPath('home'),
        configDir: getConfigDir(),
        dlcDir: getDLCDir(),
        pluginsDir: getPluginsDir(),
        cacheBase: cb,
        torchHome: process.env.TORCH_HOME || path.join(cb, 'torch'),
        hfHome: process.env.HF_HOME || path.join(cb, 'huggingface'),
    };
}

/**
 * Stop the backend, then delete the selected config paths. Async so we can give
 * the killed Python process a moment to release file/DB handles (mainly Windows,
 * where an open handle blocks deletion) before unlinking.
 */
export async function resetConfig(selection: ResetSelection): Promise<ResetSummary> {
    const env = buildConfigPathEnv();
    const cats = enumerateConfigPaths(env);
    const targets = buildDeleteSet(selection, cats);

    // Nothing selected (malformed/all-false payload): no-op. Crucially, do NOT
    // stop the backend — otherwise an empty request would leave the app broken
    // until a manual restart for no benefit.
    if (targets.length === 0) {
        console.log('[config-reset] empty selection — nothing to reset; backend left running');
        return { selection, configDir: env.configDir, deleted: [] };
    }

    // Hard-stop the backend (SIGKILL group) so it isn't holding SQLite/WAL files
    // while we delete them. WAL makes an abrupt stop crash-safe, and we restart
    // right after, so abandoning in-flight work is intended.
    try {
        stopPython(true);
    } catch (err) {
        console.warn(`[config-reset] stopPython failed (continuing): ${String(err)}`);
    }
    // Give the OS a beat to actually reap the process and close its handles.
    await new Promise((r) => setTimeout(r, 600));

    // Chromium state / Crashpad are held open by this live process — Chromium
    // rewrites them on quit and Windows blocks deleting open files, so deleting
    // them now could "succeed" yet leave the state intact. Delete everything else
    // immediately and defer those to the next launch (config-bootstrap applies the
    // manifest before any window / crashReporter reopens them).
    const { immediate, deferred } = partitionDeferred(targets);
    const deleted = deletePaths(immediate);

    const scheduled = schedulePendingDeletion(env.userData, deferred);
    const deferredEntries: ResetEntry[] = deferred.map((p) => {
        let existed = false;
        try { existed = fs.existsSync(p); } catch { /* ignore */ }
        // If the manifest couldn't be written, the next launch won't delete these
        // — report them as failures rather than a false "Restart to finish".
        return scheduled
            ? { path: p, existed, ok: true, deferred: true }
            : { path: p, existed, ok: false, deferred: true, error: 'could not schedule deferred deletion' };
    });

    const allEntries = [...deleted, ...deferredEntries];
    const summary: ResetSummary = { selection, configDir: env.configDir, deleted: allEntries };
    const failed = deleted.filter((d) => !d.ok);
    console.log(
        `[config-reset] reset done: ${allEntries.length} target(s), `
        + `${deleted.filter((d) => d.existed && d.ok).length} removed now, `
        + `${deferred.length} scheduled for next launch, ${failed.length} failed`,
    );
    if (failed.length) {
        for (const f of failed) console.warn(`[config-reset] failed to delete ${f.path}: ${f.error}`);
    }
    return summary;
}

/**
 * Register the maintenance IPC handlers. Call once from startup().
 * @param getMainWindow returns the window to parent the native confirm dialog to.
 */
export function registerMaintenanceHandlers(getMainWindow?: () => BrowserWindow | null): void {
    ipcMain.handle(IPC_MAINTENANCE_GET_PATHS, () => {
        const env = buildConfigPathEnv();
        const categories = enumerateConfigPaths(env);
        return {
            configDir: env.configDir,
            userData: env.userData,
            dlcDir: env.dlcDir,
            pluginsDir: env.pluginsDir,
            sharedDockerConfig: isSharedDockerConfig(env),
            categories,
        };
    });

    ipcMain.handle(IPC_MAINTENANCE_RESET, async (_event, selection: ResetSelection): Promise<ResetSummary> => {
        // Coerce to a known-good shape so a malformed payload can't widen scope.
        const safe: ResetSelection = {
            appSettings: selection?.appSettings === true,
            pluginState: selection?.pluginState === true,
            fullReset: selection?.fullReset === true,
            alsoInstalledPlugins: selection?.alsoInstalledPlugins === true,
            alsoSongLibrary: selection?.alsoSongLibrary === true,
            alsoMlCaches: selection?.alsoMlCaches === true,
        };

        const env = buildConfigPathEnv();
        const anySelected = safe.appSettings || safe.pluginState || safe.fullReset
            || safe.alsoInstalledPlugins || safe.alsoSongLibrary || safe.alsoMlCaches;
        if (!anySelected) {
            return { selection: safe, configDir: env.configDir, deleted: [] };
        }

        // SECURITY: this bridge is reachable by any renderer script (incl.
        // community plugins), so the renderer-side confirm is NOT a sufficient
        // gate. Require a native, main-process confirmation before deleting
        // anything — destructive (and especially the library/plugin opt-in)
        // resets must have explicit user intent that renderer content can't fake.
        const { message, detail } = describeSelection(safe);
        const parent = getMainWindow?.() ?? null;
        const opts = {
            type: 'warning' as const,
            buttons: ['Cancel', 'Reset'],
            defaultId: 0, // default to the safe choice
            cancelId: 0,
            title: 'fee[dB]ack',
            message,
            detail,
            noLink: true,
        };
        const { response } = parent && !parent.isDestroyed()
            ? await dialog.showMessageBox(parent, opts)
            : await dialog.showMessageBox(opts);
        if (response !== 1) {
            return { selection: safe, configDir: env.configDir, deleted: [], canceled: true };
        }

        return resetConfig(safe);
    });

    ipcMain.handle(IPC_MAINTENANCE_RESTART, () => {
        app.relaunch();
        app.quit();
        return { restarting: true };
    });
}
