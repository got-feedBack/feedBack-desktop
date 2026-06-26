// Versioned config migration framework. Replaces the old "delete the config
// folder before upgrading" instruction with targeted, ordered, idempotent
// migrations stamped into CONFIG_DIR/config_version.json.
//
// On startup main.ts calls runConfigMigrations(getConfigDir(), app.getVersion()).
// Each migration is run at most once (gated by the persisted schemaVersion),
// in ascending version order, and each is wrapped so a throwing migration is
// logged and skipped rather than crashing startup (Constitution VII fail-soft).
//
// PURE/INJECTABLE: the runner takes the configDir + registry as arguments and
// touches only the filesystem, so it unit-tests without electron. Register real
// migrations by appending to MIGRATIONS and bumping CURRENT_SCHEMA_VERSION.

import * as fs from 'fs';
import * as path from 'path';

// Bump this (and append a matching MIGRATIONS entry) whenever the on-disk config
// schema changes in a way that needs a one-time fix-up.
export const CURRENT_SCHEMA_VERSION = 1;

export interface MigrationContext {
    /** Active backend CONFIG_DIR the migration may read/modify. */
    configDir: string;
    /** App version running the migration (for logging / conditional fixes). */
    appVersion: string;
}

export interface Migration {
    /** Target schema version this migration brings the config UP TO. */
    version: number;
    /** Short human-readable name for logs. */
    name: string;
    /** Idempotent fix-up. Must tolerate partial/already-applied state. */
    run(ctx: MigrationContext): void;
}

// Ordered registry. v1 is an intentional no-op baseline: it just establishes the
// stamp on installs that predate this framework, so future migrations have a
// known floor. Real migrations append here with version 2, 3, ….
export const MIGRATIONS: Migration[] = [
    {
        version: 1,
        name: 'baseline-stamp',
        run: () => {
            /* no-op: establishes config_version.json at v1 */
        },
    },
];

interface VersionStamp {
    schemaVersion: number;
    appVersion: string;
    updatedAt: string;
}

const STAMP_FILE = 'config_version.json';

function stampPath(configDir: string): string {
    return path.join(configDir, STAMP_FILE);
}

/** Read the persisted schema version; 0 if absent or unreadable (treat a
 *  corrupt/missing stamp as "pre-framework" so migrations re-run safely). */
export function readSchemaVersion(configDir: string): number {
    try {
        const raw = fs.readFileSync(stampPath(configDir), 'utf-8');
        const parsed = JSON.parse(raw) as Partial<VersionStamp>;
        const v = Number(parsed.schemaVersion);
        return Number.isInteger(v) && v >= 0 ? v : 0;
    } catch {
        return 0;
    }
}

function writeStamp(configDir: string, schemaVersion: number, appVersion: string, now: string): void {
    const stamp: VersionStamp = { schemaVersion, appVersion, updatedAt: now };
    try {
        fs.mkdirSync(configDir, { recursive: true });
        // Atomic-ish write so a crash mid-write can't leave a truncated stamp.
        const tmp = stampPath(configDir) + '.tmp';
        fs.writeFileSync(tmp, JSON.stringify(stamp, null, 2));
        fs.renameSync(tmp, stampPath(configDir));
    } catch (err) {
        console.warn(`[config-migrations] failed to write ${STAMP_FILE}: ${String(err)}`);
    }
}

export interface MigrationResult {
    from: number;
    to: number;
    ran: { version: number; name: string; ok: boolean; error?: string }[];
}

/**
 * Run every registered migration whose version is > the persisted schema version
 * and <= CURRENT_SCHEMA_VERSION, in ascending order, then update the stamp.
 *
 * - Idempotent: a second call with an up-to-date stamp runs nothing.
 * - Fail-soft: a throwing migration is logged and skipped; later migrations
 *   still run. The stamp still advances to CURRENT_SCHEMA_VERSION so a single
 *   persistently-failing migration can't wedge startup forever (it's surfaced
 *   in the returned result / logs instead).
 *
 * `now` is injectable for deterministic tests (Date is unavailable in some
 * sandboxes); callers in main.ts pass new Date().toISOString().
 */
export function runConfigMigrations(
    configDir: string,
    appVersion: string,
    now: string,
    registry: Migration[] = MIGRATIONS,
): MigrationResult {
    const from = readSchemaVersion(configDir);
    const result: MigrationResult = { from, to: from, ran: [] };

    if (from >= CURRENT_SCHEMA_VERSION) {
        console.log(`[config-migrations] schema up to date (v${from}) at ${configDir}`);
        return result;
    }

    const pending = registry
        .filter((m) => m.version > from && m.version <= CURRENT_SCHEMA_VERSION)
        .sort((a, b) => a.version - b.version);

    for (const m of pending) {
        try {
            m.run({ configDir, appVersion });
            result.ran.push({ version: m.version, name: m.name, ok: true });
            console.log(`[config-migrations] applied v${m.version} (${m.name})`);
        } catch (err) {
            const error = String(err);
            result.ran.push({ version: m.version, name: m.name, ok: false, error });
            console.warn(`[config-migrations] migration v${m.version} (${m.name}) failed, skipping: ${error}`);
        }
    }

    writeStamp(configDir, CURRENT_SCHEMA_VERSION, appVersion, now);
    result.to = CURRENT_SCHEMA_VERSION;
    console.log(`[config-migrations] config schema v${from} → v${CURRENT_SCHEMA_VERSION} at ${configDir}`);
    return result;
}
