import * as fs from 'fs';
import * as path from 'path';

export type LibraryPathPreparationStatus =
    | 'explicit-override'
    | 'configured'
    | 'bootstrapped'
    | 'invalid-config'
    | 'write-failed';

export interface LibraryPathPreparation {
    status: LibraryPathPreparationStatus;
    environmentDlcDir?: string;
    error?: string;
}

function isPlainObject(value: unknown): value is Record<string, unknown> {
    return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/**
 * Normalize a configured library path and accept it only when it already
 * names a directory.
 */
export function normalizeExistingLibraryDirectory(rawPath?: string): string | undefined {
    const candidate = (rawPath || '').trim();
    if (!candidate) return undefined;

    try {
        return fs.statSync(candidate).isDirectory() ? candidate : undefined;
    } catch {
        return undefined;
    }
}

/**
 * Apply the prepared library-path contract to the Python child environment.
 * Starting from a copy of process.env means an invalid or stale parent value
 * must be removed explicitly when config.json owns the path.
 */
export function applyLibraryPathToPythonEnvironment(
    environment: Record<string, string>,
    preparation: LibraryPathPreparation,
): void {
    if (preparation.environmentDlcDir) {
        environment.DLC_DIR = preparation.environmentDlcDir;
    } else {
        delete environment.DLC_DIR;
    }
}

/**
 * Prepare the library path contract for the Python backend.
 *
 * An explicit DLC_DIR remains an administrator-owned environment override.
 * Normal desktop launches instead keep the selected path in config.json so
 * the backend can re-read a Settings change on the next manual scan without
 * requiring a process restart.
 *
 * Failure statuses (invalid-config, write-failed) still export the resolved
 * fallback as DLC_DIR: the backend has NO built-in default (_get_dlc_dir
 * returns None), so removing the env var when config.json cannot own the
 * path would strand the user with an empty library and only a console warn.
 * Config-owned dynamic behaviour applies exactly when config.json is usable.
 */
export function prepareLibraryPathForPython(
    configDir: string,
    resolvedDlcDir: string,
    explicitDlcDir?: string,
): LibraryPathPreparation {
    const override = normalizeExistingLibraryDirectory(explicitDlcDir);
    if (override) {
        return {
            status: 'explicit-override',
            environmentDlcDir: override,
        };
    }

    const configFile = path.join(configDir, 'config.json');
    let config: Record<string, unknown> = {};

    if (fs.existsSync(configFile)) {
        try {
            const parsed: unknown = JSON.parse(fs.readFileSync(configFile, 'utf8'));
            if (!isPlainObject(parsed)) {
                return {
                    status: 'invalid-config',
                    environmentDlcDir: resolvedDlcDir,
                    error: 'config.json is not a JSON object',
                };
            }
            config = parsed;
        } catch (err) {
            return {
                status: 'invalid-config',
                environmentDlcDir: resolvedDlcDir,
                error: err instanceof Error ? err.message : String(err),
            };
        }

        const configured = config.dlc_dir;
        if (typeof configured === 'string' && configured.trim()) {
            if (normalizeExistingLibraryDirectory(configured)) {
                return { status: 'configured' };
            }
            return {
                status: 'invalid-config',
                environmentDlcDir: resolvedDlcDir,
                error: 'config.json dlc_dir is not an existing directory',
            };
        }
        if (configured !== undefined && configured !== null && configured !== '') {
            return {
                status: 'invalid-config',
                environmentDlcDir: resolvedDlcDir,
                error: 'config.json dlc_dir is not a string',
            };
        }
    }

    config.dlc_dir = resolvedDlcDir;
    const tmpFile = configFile + '.tmp';
    try {
        fs.mkdirSync(configDir, { recursive: true });
        fs.writeFileSync(tmpFile, JSON.stringify(config, null, 2), 'utf8');
        fs.renameSync(tmpFile, configFile);
    } catch (err) {
        try {
            if (fs.existsSync(tmpFile)) fs.rmSync(tmpFile, { force: true });
        } catch { /* best-effort temporary-file cleanup */ }
        return {
            status: 'write-failed',
            environmentDlcDir: resolvedDlcDir,
            error: err instanceof Error ? err.message : String(err),
        };
    }

    return { status: 'bootstrapped' };
}
