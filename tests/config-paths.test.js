const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const { loadTs } = require('./_load-ts');

const {
    enumerateConfigPaths,
    buildDeleteSet,
    partitionDeferred,
    isSharedDockerConfig,
    DEFERRED_BASENAMES,
} = loadTs('src/main/config-paths.ts');

// Per-OS resolved path envs. enumerateConfigPaths builds everything from these
// values, so we simulate each platform by passing OS-shaped paths.
const ENVS = {
    linux: {
        platform: 'linux',
        userData: '/home/u/.config/feedback-desktop',
        home: '/home/u',
        configDir: '/home/u/.config/feedback-desktop/slopsmith-config',
        dlcDir: '/home/u/Music/Slopsmith',
        pluginsDir: '/home/u/.config/feedback-desktop/plugins',
        cacheBase: '/home/u/.cache',
        torchHome: '/home/u/.cache/torch',
        hfHome: '/home/u/.cache/huggingface',
    },
    darwin: {
        platform: 'darwin',
        userData: '/Users/u/Library/Application Support/feedback-desktop',
        home: '/Users/u',
        configDir: '/Users/u/Library/Application Support/feedback-desktop/slopsmith-config',
        dlcDir: '/Users/u/Music/Slopsmith',
        pluginsDir: '/Users/u/Library/Application Support/feedback-desktop/plugins',
        cacheBase: '/Users/u/.cache',
        torchHome: '/Users/u/.cache/torch',
        hfHome: '/Users/u/.cache/huggingface',
    },
    win32: {
        platform: 'win32',
        userData: '/c/Users/u/AppData/Roaming/feedback-desktop',
        home: '/c/Users/u',
        configDir: '/c/Users/u/AppData/Roaming/feedback-desktop/slopsmith-config',
        dlcDir: '/c/Users/u/Music/Slopsmith',
        pluginsDir: '/c/Users/u/AppData/Roaming/feedback-desktop/plugins',
        cacheBase: '/c/Users/u/.cache',
        torchHome: '/c/Users/u/.cache/torch',
        hfHome: '/c/Users/u/.cache/huggingface',
    },
};

test('enumerateConfigPaths places known desktop state under userData (each OS)', () => {
    for (const [name, env] of Object.entries(ENVS)) {
        const cats = enumerateConfigPaths(env);
        const u = env.userData;
        const c = env.configDir;
        for (const expected of [
            path.join(u, 'slopsmith-desktop.json'),
            path.join(u, 'slopsmith-audio-settings.json'),
            path.join(u, 'soundfonts'),
            path.join(u, 'vst-load-sentinel.json'),
            path.join(u, 'vst-crash-blocklist.json'),
            path.join(u, 'known-plugins.xml'),
            path.join(u, 'Crashpad'),
        ]) {
            assert.ok(cats.appSettingsAndCaches.includes(expected), `${name}: missing ${expected}`);
        }
        assert.ok(cats.pluginStateAndPyDeps.includes(path.join(c, 'plugin_state.json')), `${name}: plugin_state`);
        assert.ok(cats.pluginStateAndPyDeps.includes(path.join(c, 'pip_packages')), `${name}: pip_packages`);
        assert.ok(cats.configDbsAndState.includes(path.join(c, 'web_library.db')), `${name}: web_library.db`);
        assert.ok(cats.configDbsAndState.includes(path.join(c, 'config.json')), `${name}: config.json`);
        // WAL/SHM sidecars must be cleared alongside each DB.
        assert.ok(cats.configDbsAndState.includes(path.join(c, 'web_library.db-wal')), `${name}: db-wal`);
        assert.ok(cats.configDbsAndState.includes(path.join(c, 'web_library.db-shm')), `${name}: db-shm`);
        // The migration stamp is part of a full reset so migrations re-run after.
        assert.ok(cats.configDbsAndState.includes(path.join(c, 'config_version.json')), `${name}: stamp`);
    }
});

test('buildDeleteSet returns an empty set for an empty/all-false selection', () => {
    const cats = enumerateConfigPaths(ENVS.linux);
    assert.deepEqual(buildDeleteSet({}, cats), []);
    assert.deepEqual(buildDeleteSet({ appSettings: false, fullReset: false }, cats), []);
});

test('partitionDeferred routes Chromium-held paths to deferred, the rest to immediate', () => {
    const env = ENVS.linux;
    const cats = enumerateConfigPaths(env);
    const { immediate, deferred } = partitionDeferred(buildDeleteSet({ fullReset: true }, cats));

    // Every deferred path has a Chromium/Crashpad basename.
    for (const p of deferred) {
        assert.ok(DEFERRED_BASENAMES.includes(path.basename(p)), `unexpected deferred ${p}`);
    }
    // Crashpad + the 5 Electron-state dirs are deferred; nothing else is.
    assert.deepEqual(
        deferred.map((p) => path.basename(p)).sort(),
        [...DEFERRED_BASENAMES].sort(),
    );
    // DBs and prefs go immediate, never deferred.
    assert.ok(immediate.includes(path.join(env.configDir, 'web_library.db')));
    assert.ok(immediate.includes(path.join(env.userData, 'slopsmith-desktop.json')));
    assert.ok(!immediate.some((p) => DEFERRED_BASENAMES.includes(path.basename(p))));
});

test('SAFETY: song library, installed plugins and ML caches are ONLY in optInExtras', () => {
    for (const [name, env] of Object.entries(ENVS)) {
        const cats = enumerateConfigPaths(env);
        const safe = [
            ...cats.appSettingsAndCaches,
            ...cats.pluginStateAndPyDeps,
            ...cats.configDbsAndState,
        ];
        // None of the safe categories may equal or be a child of the protected dirs.
        const protectedRoots = [
            env.dlcDir,
            env.pluginsDir,
            path.join(env.cacheBase, 'torch'),
            path.join(env.cacheBase, 'huggingface'),
        ];
        for (const root of protectedRoots) {
            assert.ok(!safe.includes(root), `${name}: ${root} leaked into a safe category`);
            assert.ok(
                !safe.some((p) => p === root || p.startsWith(root + path.sep)),
                `${name}: a safe path lives under protected ${root}`,
            );
        }
        // And they ARE present in optInExtras.
        assert.deepEqual(cats.optInExtras.songLibrary, [env.dlcDir], `${name}: songLibrary`);
        assert.deepEqual(cats.optInExtras.installedPlugins, [env.pluginsDir], `${name}: installedPlugins`);
        assert.deepEqual(
            cats.optInExtras.mlCaches,
            [path.join(env.cacheBase, 'torch'), path.join(env.cacheBase, 'huggingface')],
            `${name}: mlCaches`,
        );
    }
});

test('buildDeleteSet honors flags and never widens to opt-in extras implicitly', () => {
    const env = ENVS.linux;
    const cats = enumerateConfigPaths(env);
    const extras = [
        env.dlcDir,
        env.pluginsDir,
        ...cats.optInExtras.mlCaches,
    ];

    const appOnly = buildDeleteSet({ appSettings: true }, cats);
    assert.deepEqual(appOnly, cats.appSettingsAndCaches);
    extras.forEach((p) => assert.ok(!appOnly.includes(p), `appSettings leaked ${p}`));

    const pluginOnly = buildDeleteSet({ pluginState: true }, cats);
    assert.deepEqual(pluginOnly, cats.pluginStateAndPyDeps);

    const full = buildDeleteSet({ fullReset: true }, cats);
    for (const p of [...cats.appSettingsAndCaches, ...cats.pluginStateAndPyDeps, ...cats.configDbsAndState]) {
        assert.ok(full.includes(p), `fullReset missing ${p}`);
    }
    extras.forEach((p) => assert.ok(!full.includes(p), `fullReset leaked ${p} without opt-in`));

    const fullPlusAll = buildDeleteSet(
        { fullReset: true, alsoInstalledPlugins: true, alsoSongLibrary: true, alsoMlCaches: true },
        cats,
    );
    extras.forEach((p) => assert.ok(fullPlusAll.includes(p), `opt-in missing ${p}`));

    const installedOnly = buildDeleteSet({ alsoInstalledPlugins: true }, cats);
    assert.deepEqual(installedOnly, [env.pluginsDir]);

    // De-duplication: app + full must not double-list shared app paths.
    const merged = buildDeleteSet({ appSettings: true, fullReset: true }, cats);
    assert.equal(merged.length, new Set(merged).size);
});

test('mlCaches honors custom TORCH_HOME / HF_HOME locations', () => {
    const env = {
        ...ENVS.linux,
        torchHome: '/mnt/big/torch',
        hfHome: '/mnt/big/hf',
    };
    const cats = enumerateConfigPaths(env);
    assert.deepEqual(cats.optInExtras.mlCaches, ['/mnt/big/torch', '/mnt/big/hf']);
});

test('isSharedDockerConfig detects the Linux shared ~/.local/share/slopsmith dir', () => {
    assert.equal(
        isSharedDockerConfig({ ...ENVS.linux, configDir: '/home/u/.local/share/slopsmith' }),
        true,
    );
    assert.equal(isSharedDockerConfig(ENVS.linux), false);
});
