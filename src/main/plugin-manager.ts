// Plugin Manager — handles installation, removal, and updates
// of Slopsmith plugins via git operations.

import { ipcMain } from 'electron';
import { execFile } from 'child_process';
import * as path from 'path';
import * as fs from 'fs';
import { getPluginsDir, restartPython } from './python';

// Run git with an explicit argv array — never via a shell. This removes the
// OS command-injection vector that `exec(`git clone ${gitUrl} ...`)` had:
// gitUrl/name are no longer interpolated into a shell string.
function execFileAsync(file: string, args: string[], cwd?: string): Promise<string> {
    return new Promise((resolve, reject) => {
        execFile(file, args, { cwd, timeout: 60000 }, (error, stdout, stderr) => {
            if (error) reject(new Error(stderr || error.message));
            else resolve(stdout.trim());
        });
    });
}

// Plugin directory names are a single path segment directly under the
// plugins dir. Reject separators, traversal, and leading dot/dash so a
// renderer-supplied `name` can't escape the plugins dir (which would let
// remove/update/install operate on an arbitrary directory).
const SAFE_PLUGIN_NAME = /^[A-Za-z0-9][A-Za-z0-9._-]*$/;
function resolveSafePluginDir(pluginsDir: string, name: string): string | null {
    // `name` arrives over IPC and may not be a string; guard before
    // path.resolve (which throws on non-string args).
    if (typeof name !== 'string' || !name || !SAFE_PLUGIN_NAME.test(name)) return null;
    const root = path.resolve(pluginsDir);
    const target = path.resolve(root, name);
    const rel = path.relative(root, target);
    if (rel === '' || rel.startsWith('..') || path.isAbsolute(rel) || rel.includes(path.sep)) {
        return null;
    }
    return target;
}

// Require a well-formed https:// URL with a hostname so a renderer can't
// point git at a local path / file:// / ext:: transport (or a malformed
// `https:///` with no host). Parsing with URL also rejects whitespace and
// junk a bare prefix regex would let through. (Shell injection is already
// gone via execFile — this is transport/host hardening.)
function isValidGitUrl(url: string): boolean {
    try {
        const u = new URL(url);
        return u.protocol === 'https:' && u.hostname.length > 0;
    } catch {
        return false;
    }
}

interface InstalledPlugin {
    name: string;
    path: string;
    hasGit: boolean;
    manifest: any | null;
    version: string;
}

async function listInstalledPlugins(): Promise<InstalledPlugin[]> {
    const pluginsDir = getPluginsDir();
    const plugins: InstalledPlugin[] = [];

    if (!fs.existsSync(pluginsDir)) return plugins;

    const entries = fs.readdirSync(pluginsDir, { withFileTypes: true });
    for (const entry of entries) {
        if (entry.name.startsWith('.')) continue;

        const pluginPath = path.join(pluginsDir, entry.name);

        // Accept real directories and symlinks that resolve to a directory.
        // The README documents symlinking a plugin repo into the plugins
        // dir, but Dirent.isDirectory() is false for a symlink-to-dir, so
        // stat the resolved path instead. statSync throws on a broken
        // symlink (or unreadable entry) — skip those.
        let isDir = false;
        try {
            isDir = fs.statSync(pluginPath).isDirectory();
        } catch { /* broken symlink or unreadable entry */ }
        if (!isDir) continue;
        const manifestPath = path.join(pluginPath, 'plugin.json');
        const gitDir = path.join(pluginPath, '.git');

        let manifest = null;
        try {
            if (fs.existsSync(manifestPath)) {
                manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf-8'));
            }
        } catch { /* invalid manifest */ }

        let version = manifest?.version || 'unknown';

        // Try to get git version info
        if (fs.existsSync(gitDir)) {
            try {
                const hash = await execFileAsync('git', ['rev-parse', '--short', 'HEAD'], pluginPath);
                version = `${version} (${hash})`;
            } catch { /* not a git repo */ }
        }

        plugins.push({
            name: entry.name,
            path: pluginPath,
            hasGit: fs.existsSync(gitDir),
            manifest,
            version,
        });
    }

    return plugins;
}

async function installPlugin(gitUrl: string, name?: string): Promise<{ success: boolean; message: string }> {
    const pluginsDir = getPluginsDir();

    if (typeof gitUrl !== 'string' || !isValidGitUrl(gitUrl)) {
        return { success: false, message: 'Invalid git URL — only https:// remotes are allowed' };
    }

    // Derive directory name from URL if not provided
    if (!name) {
        // https://github.com/user/slopsmith-plugin-foo.git -> slopsmith-plugin-foo
        const urlParts = gitUrl.replace(/\.git$/, '').split('/');
        name = urlParts[urlParts.length - 1] || 'plugin';
    }

    const targetDir = resolveSafePluginDir(pluginsDir, name);
    if (!targetDir) {
        return { success: false, message: `Invalid plugin name "${name}"` };
    }

    if (fs.existsSync(targetDir)) {
        return { success: false, message: `Plugin directory "${name}" already exists` };
    }

    try {
        // Partial, checkout-less clone: fetches commit/tree objects but not
        // blob contents, so we can confirm plugin.json exists at the repo
        // root before paying for a full checkout — or aborting entirely
        // without ever fetching the plugin's file contents.
        await execFileAsync('git', ['clone', '--depth', '1', '--filter=blob:none', '--no-checkout', gitUrl, targetDir]);

        let hasManifest = false;
        try {
            const out = await execFileAsync('git', ['ls-tree', '--name-only', 'HEAD', '--', 'plugin.json'], targetDir);
            hasManifest = out.trim() === 'plugin.json';
        } catch { /* treat as missing */ }

        if (!hasManifest) {
            fs.rmSync(targetDir, { recursive: true });
            return { success: false, message: `"${name}" has no plugin.json at its root — not a valid plugin. Install aborted.` };
        }

        // Manifest confirmed present — now check out the working tree
        // (this is where blob content actually gets fetched).
        await execFileAsync('git', ['checkout', 'HEAD', '--', '.'], targetDir);

        return { success: true, message: `Installed "${name}" successfully. Restart to activate.` };
    } catch (e: any) {
        // Clean up failed clone
        try { fs.rmSync(targetDir, { recursive: true }); } catch { /* ignore */ }
        return { success: false, message: `Failed to clone: ${e.message}` };
    }
}

async function removePlugin(name: string): Promise<{ success: boolean; message: string }> {
    const pluginsDir = getPluginsDir();
    const targetDir = resolveSafePluginDir(pluginsDir, name);
    if (!targetDir) {
        return { success: false, message: `Invalid plugin name "${name}"` };
    }

    if (!fs.existsSync(targetDir)) {
        return { success: false, message: `Plugin "${name}" not found` };
    }

    try {
        fs.rmSync(targetDir, { recursive: true });
        return { success: true, message: `Removed "${name}". Restart to take effect.` };
    } catch (e: any) {
        return { success: false, message: `Failed to remove: ${e.message}` };
    }
}

async function updatePlugin(name: string): Promise<{ success: boolean; message: string }> {
    const pluginsDir = getPluginsDir();
    const targetDir = resolveSafePluginDir(pluginsDir, name);
    if (!targetDir) {
        return { success: false, message: `Invalid plugin name "${name}"` };
    }

    if (!fs.existsSync(targetDir)) {
        return { success: false, message: `Plugin "${name}" not found` };
    }

    if (!fs.existsSync(path.join(targetDir, '.git'))) {
        return { success: false, message: `Plugin "${name}" is not a git repository — cannot update` };
    }

    try {
        const output = await execFileAsync('git', ['pull'], targetDir);
        if (output.includes('Already up to date')) {
            return { success: true, message: `"${name}" is already up to date` };
        }
        return { success: true, message: `Updated "${name}". Restart to activate changes.` };
    } catch (e: any) {
        return { success: false, message: `Failed to update: ${e.message}` };
    }
}

export function initPluginManager(): void {
    ipcMain.handle('plugins:listInstalled', async () => {
        return await listInstalledPlugins();
    });

    ipcMain.handle('plugins:install', async (_event, gitUrl: string, name?: string) => {
        return await installPlugin(gitUrl, name);
    });

    ipcMain.handle('plugins:remove', async (_event, name: string) => {
        return await removePlugin(name);
    });

    ipcMain.handle('plugins:update', async (_event, name: string) => {
        return await updatePlugin(name);
    });

    ipcMain.handle('plugins:restart', () => {
        restartPython();
        return { success: true, message: 'Restarting server...' };
    });
}
