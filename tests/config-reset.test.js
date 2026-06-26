const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { loadTs } = require('./_load-ts');

// resetConfig itself is electron/python-bound, but its delete logic is the pure
// enumerateConfigPaths → buildDeleteSet → deletePaths pipeline (config-paths.ts).
// We exercise that pipeline against a real on-disk fake tree to prove the
// "library & plugins preserved" guarantees end-to-end.
const { enumerateConfigPaths, buildDeleteSet, deletePaths } = loadTs('src/main/config-paths.ts');

function buildFakeTree() {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'feedback-reset-'));
    const userData = path.join(root, 'feedback-desktop');
    const configDir = path.join(userData, 'slopsmith-config');
    const pluginsDir = path.join(userData, 'plugins');
    const dlcDir = path.join(root, 'Library'); // song library lives OUTSIDE userData
    const cacheBase = path.join(root, '.cache');

    const write = (p, c = 'x') => {
        fs.mkdirSync(path.dirname(p), { recursive: true });
        fs.writeFileSync(p, c);
    };

    // App settings & caches
    write(path.join(userData, 'slopsmith-desktop.json'));
    write(path.join(userData, 'slopsmith-audio-settings.json'));
    write(path.join(userData, 'soundfonts', 'FluidR3_GM.sf2'));
    write(path.join(userData, 'known-plugins.xml'));
    // Plugin state & python deps
    write(path.join(configDir, 'plugin_state.json'));
    write(path.join(configDir, 'pip_packages', 'somepkg', '__init__.py'));
    write(path.join(configDir, 'plugin_data', 'foo.json'));
    // Backend DBs + config (incl. WAL/SHM sidecars left by an abrupt stop)
    write(path.join(configDir, 'web_library.db'));
    write(path.join(configDir, 'web_library.db-wal'));
    write(path.join(configDir, 'web_library.db-shm'));
    write(path.join(configDir, 'config.json'));
    write(path.join(configDir, 'config_version.json'));
    // Protected: installed plugin, song library, ML caches
    write(path.join(pluginsDir, 'my-plugin', 'plugin.json'));
    write(path.join(dlcDir, 'song.psarc'));
    write(path.join(cacheBase, 'torch', 'model.pt'));
    write(path.join(cacheBase, 'huggingface', 'blob'));

    const env = {
        platform: process.platform,
        userData,
        home: root,
        configDir,
        dlcDir,
        pluginsDir,
        cacheBase,
        torchHome: path.join(cacheBase, 'torch'),
        hfHome: path.join(cacheBase, 'huggingface'),
    };
    return { root, env, userData, configDir, pluginsDir, dlcDir, cacheBase };
}

function run(selection, env) {
    const cats = enumerateConfigPaths(env);
    return deletePaths(buildDeleteSet(selection, cats));
}

function protectedIntact(t) {
    assert.ok(fs.existsSync(path.join(t.pluginsDir, 'my-plugin', 'plugin.json')), 'installed plugin preserved');
    assert.ok(fs.existsSync(path.join(t.dlcDir, 'song.psarc')), 'song library preserved');
    assert.ok(fs.existsSync(path.join(t.cacheBase, 'torch', 'model.pt')), 'ML cache preserved');
}

test('appSettings reset removes desktop prefs/caches but preserves DBs, plugins and library', () => {
    const t = buildFakeTree();
    run({ appSettings: true }, t.env);
    assert.ok(!fs.existsSync(path.join(t.userData, 'slopsmith-desktop.json')), 'pref deleted');
    assert.ok(!fs.existsSync(path.join(t.userData, 'soundfonts')), 'soundfont cache deleted');
    assert.ok(fs.existsSync(path.join(t.configDir, 'web_library.db')), 'DB preserved');
    assert.ok(fs.existsSync(path.join(t.configDir, 'plugin_state.json')), 'plugin state preserved');
    protectedIntact(t);
});

test('pluginState reset clears plugin state/py-deps but keeps installed plugins, DBs and library', () => {
    const t = buildFakeTree();
    run({ pluginState: true }, t.env);
    assert.ok(!fs.existsSync(path.join(t.configDir, 'plugin_state.json')), 'plugin_state deleted');
    assert.ok(!fs.existsSync(path.join(t.configDir, 'pip_packages')), 'pip_packages deleted');
    assert.ok(!fs.existsSync(path.join(t.configDir, 'plugin_data')), 'plugin_data deleted');
    assert.ok(fs.existsSync(path.join(t.configDir, 'web_library.db')), 'DB preserved');
    assert.ok(fs.existsSync(path.join(t.userData, 'slopsmith-desktop.json')), 'app prefs preserved');
    protectedIntact(t);
});

test('fullReset (no opt-ins) wipes config DBs but still preserves library and installed plugins', () => {
    const t = buildFakeTree();
    run({ fullReset: true }, t.env);
    assert.ok(!fs.existsSync(path.join(t.configDir, 'web_library.db')), 'DB deleted');
    assert.ok(!fs.existsSync(path.join(t.configDir, 'web_library.db-wal')), 'WAL sidecar deleted');
    assert.ok(!fs.existsSync(path.join(t.configDir, 'web_library.db-shm')), 'SHM sidecar deleted');
    assert.ok(!fs.existsSync(path.join(t.configDir, 'config.json')), 'config.json deleted');
    assert.ok(!fs.existsSync(path.join(t.configDir, 'config_version.json')), 'migration stamp deleted');
    assert.ok(!fs.existsSync(path.join(t.userData, 'slopsmith-desktop.json')), 'app prefs deleted');
    assert.ok(!fs.existsSync(path.join(t.configDir, 'plugin_state.json')), 'plugin state deleted');
    protectedIntact(t);
});

test('opt-in flags remove the protected trees', () => {
    const t = buildFakeTree();
    run(
        { fullReset: true, alsoInstalledPlugins: true, alsoSongLibrary: true, alsoMlCaches: true },
        t.env,
    );
    assert.ok(!fs.existsSync(t.pluginsDir), 'installed plugins removed on opt-in');
    assert.ok(!fs.existsSync(t.dlcDir), 'song library removed on opt-in');
    assert.ok(!fs.existsSync(path.join(t.cacheBase, 'torch')), 'torch cache removed on opt-in');
    assert.ok(!fs.existsSync(path.join(t.cacheBase, 'huggingface')), 'hf cache removed on opt-in');
});

test('deletePaths is fail-soft on a non-existent path', () => {
    const t = buildFakeTree();
    const missing = path.join(t.root, 'does-not-exist');
    const [entry] = deletePaths([missing]);
    assert.equal(entry.ok, true);
    assert.equal(entry.existed, false);
});
