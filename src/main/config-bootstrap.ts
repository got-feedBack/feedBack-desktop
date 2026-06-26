// One-time userData folder migration.
//
// The app now pins its name to `feedback-desktop` (app.setName in main.ts), so
// app.getPath('userData') resolves deterministically to <appData>/feedback-desktop
// on every OS. Before this, the folder name was OS-derived and inconsistent:
//   - macOS:   <appData>/fee[dB]ack        (from build.productName)
//   - Linux:   ~/.config/slopsmith-desktop (from package.json name)
//   - Windows: %APPDATA%\slopsmith-desktop (from package.json name)
//
// To stop existing testers from "starting fresh" after the rename, on first
// launch under the new name we copy a legacy folder into the new one. Old and
// new always share the same <appData> parent, so we derive legacy candidates
// from dirname(newUserData) rather than reconstructing platform-specific roots.
//
// This MUST run before crashReporter.start() / app.whenReady() — i.e. before
// anything creates the new userData dir — because the "should I migrate?" gate
// is simply "the new dir does not exist yet".
//
// Fail-soft (Constitution VII): any error is logged and swallowed; a failed
// migration must never block launch. The copy is atomic (copy to a temp sibling,
// then rename) so a crash mid-copy can't leave a half-populated new dir that the
// gate would then treat as "already migrated".
//
// Does NOT touch the Linux shared ~/.local/share/slopsmith config or ~/.cache/*
// ML caches — those are absolute paths unaffected by the userData rename.

import * as fs from 'fs';
import * as path from 'path';
import { app } from 'electron';

// Legacy userData folder names, newest-intent first. Derived against the same
// <appData> parent as the new dir, so this works on every OS.
export const LEGACY_USERDATA_NAMES = ['fee[dB]ack', 'slopsmith-desktop'];

const MIGRATION_MARKER = 'userdata-migrated.json';

export interface UserDataMigrationResult {
    migrated: boolean;
    from?: string;
    to?: string;
    reason?: string;
}

/**
 * Pure, testable core: migrate into `newUserData` from the first existing legacy
 * sibling, but only if `newUserData` doesn't already exist.
 */
export function migrateUserData(
    newUserData: string,
    now: string,
    legacyNames: string[] = LEGACY_USERDATA_NAMES,
): UserDataMigrationResult {
    try {
        if (fs.existsSync(newUserData)) {
            return { migrated: false, reason: 'new userData already exists' };
        }

        const parent = path.dirname(newUserData);
        const newName = path.basename(newUserData);
        const legacy = legacyNames
            .filter((n) => n !== newName)
            .map((n) => path.join(parent, n))
            .find((p) => {
                try {
                    return fs.existsSync(p) && fs.statSync(p).isDirectory();
                } catch {
                    return false;
                }
            });

        if (!legacy) {
            return { migrated: false, reason: 'no legacy userData found' };
        }

        // Copy atomically: populate a temp sibling, then rename into place. If
        // the copy throws, remove the partial temp dir and bail (the new dir is
        // never created, so a retry on next launch is clean).
        const staging = newUserData + '.migrating';
        try {
            fs.rmSync(staging, { recursive: true, force: true });
        } catch {
            /* best-effort cleanup of a prior aborted attempt */
        }
        try {
            fs.cpSync(legacy, staging, { recursive: true });
            fs.writeFileSync(
                path.join(staging, MIGRATION_MARKER),
                JSON.stringify({ from: legacy, at: now }, null, 2),
            );
            fs.renameSync(staging, newUserData);
        } catch (err) {
            try {
                fs.rmSync(staging, { recursive: true, force: true });
            } catch {
                /* leave it; nothing else we can do */
            }
            throw err;
        }

        console.log(`[config-bootstrap] migrated userData ${legacy} → ${newUserData}`);
        return { migrated: true, from: legacy, to: newUserData };
    } catch (err) {
        console.warn(`[config-bootstrap] userData migration failed (continuing): ${String(err)}`);
        return { migrated: false, reason: String(err) };
    }
}

/**
 * Electron entry point. Call once, immediately after app.setName(...) and BEFORE
 * crashReporter.start() / app.whenReady().
 */
export function migrateUserDataIfNeeded(): UserDataMigrationResult {
    // app.getPath('userData') is available before `ready` and reflects the name
    // pinned by app.setName(); it does not create the directory.
    const newUserData = app.getPath('userData');
    return migrateUserData(newUserData, new Date().toISOString());
}

// ── Deferred reset deletions ─────────────────────────────────────────────────
// Some reset targets (Chromium state, Crashpad) are held open while the app runs,
// so config-reset.resetConfig() writes them to this manifest instead of deleting
// them live. consumePendingReset() applies them at the very start of the next
// launch — before any BrowserWindow or crashReporter reopens them.

const PENDING_RESET = 'pending-reset.json';

function pendingResetPath(userData: string): string {
    return path.join(userData, PENDING_RESET);
}

/** Append paths to the pending-deletion manifest (merged + de-duplicated).
 *  Returns true if the manifest is in place (or there was nothing to schedule),
 *  false if it could not be written — callers surface that so the user isn't
 *  told a deferred deletion will happen when it won't. */
export function schedulePendingDeletion(userData: string, paths: string[]): boolean {
    if (!paths.length) return true;
    const file = pendingResetPath(userData);
    let existing: string[] = [];
    try {
        if (fs.existsSync(file)) {
            const parsed = JSON.parse(fs.readFileSync(file, 'utf-8'));
            if (Array.isArray(parsed)) existing = parsed.filter((p) => typeof p === 'string');
        }
    } catch {
        /* corrupt manifest — overwrite */
    }
    const merged = [...new Set([...existing, ...paths])];
    try {
        fs.mkdirSync(userData, { recursive: true });
        const tmp = file + '.tmp';
        fs.writeFileSync(tmp, JSON.stringify(merged, null, 2));
        fs.renameSync(tmp, file);
        return true;
    } catch (err) {
        console.warn(`[config-bootstrap] failed to write ${PENDING_RESET}: ${String(err)}`);
        return false;
    }
}

/** Apply (delete) and clear any pending-deletion manifest. Returns the paths it
 *  acted on. Fail-soft: a bad manifest or a failed unlink never blocks launch. */
export function consumePendingReset(userData: string): string[] {
    const file = pendingResetPath(userData);
    let paths: string[] = [];
    try {
        if (!fs.existsSync(file)) return [];
        const parsed = JSON.parse(fs.readFileSync(file, 'utf-8'));
        paths = Array.isArray(parsed) ? parsed.filter((p) => typeof p === 'string') : [];
        for (const p of paths) {
            try {
                fs.rmSync(p, { recursive: true, force: true });
            } catch (err) {
                console.warn(`[config-bootstrap] deferred delete failed for ${p}: ${String(err)}`);
            }
        }
        fs.rmSync(file, { force: true });
        if (paths.length) {
            console.log(`[config-bootstrap] applied ${paths.length} deferred reset deletion(s)`);
        }
    } catch (err) {
        console.warn(`[config-bootstrap] consumePendingReset failed (continuing): ${String(err)}`);
    }
    return paths;
}

/** Electron entry point — call at top of main, before crashReporter / windows. */
export function consumePendingResetIfNeeded(): string[] {
    return consumePendingReset(app.getPath('userData'));
}
