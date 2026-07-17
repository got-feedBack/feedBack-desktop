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
 * Prepare the library path contract for the Python backend.
 *
 * An explicit DLC_DIR remains an administrator-owned environment override.
 * Normal desktop launches instead keep the selected path in config.json so
 * the backend can re-read a Settings change on the next manual scan without
 * requiring a process restart.
 */
export function prepareLibraryPathForPython(
    configDir: string,
    resolvedDlcDir: string,
    explicitDlcDir?: string,
): LibraryPathPreparation {
    const override = (explicitDlcDir || '').trim();
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
                    error: 'config.json is not a JSON object',
                };
            }
            config = parsed;
        } catch (err) {
            return {
                status: 'invalid-config',
                error: err instanceof Error ? err.message : String(err),
            };
        }

        const configured = config.dlc_dir;
        if (typeof configured === 'string' && configured.trim()) {
            return { status: 'configured' };
        }
        if (configured !== undefined && configured !== null && configured !== '') {
            return {
                status: 'invalid-config',
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
            error: err instanceof Error ? err.message : String(err),
        };
    }

    return { status: 'bootstrapped' };
}
