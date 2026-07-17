'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { loadTs } = require('./_load-ts');

const {
    applyLibraryPathToPythonEnvironment,
    normalizeExistingLibraryDirectory,
    prepareLibraryPathForPython,
} = loadTs('src/main/library-path-config.ts');

function tmpConfigDir() {
    return fs.mkdtempSync(path.join(os.tmpdir(), 'feedback-library-path-'));
}

test('normal desktop startup bootstraps the fallback into config instead of DLC_DIR', () => {
    const configDir = tmpConfigDir();
    const result = prepareLibraryPathForPython(configDir, 'C:\\Music\\fee[dB]ack');

    assert.deepEqual(result, { status: 'bootstrapped' });
    assert.deepEqual(
        JSON.parse(fs.readFileSync(path.join(configDir, 'config.json'), 'utf8')),
        { dlc_dir: 'C:\\Music\\fee[dB]ack' },
    );
    assert.equal(result.environmentDlcDir, undefined);
});

test('bootstrap merges the fallback into an existing config without losing settings', () => {
    const configDir = tmpConfigDir();
    const configFile = path.join(configDir, 'config.json');
    fs.writeFileSync(configFile, JSON.stringify({ master_difficulty: 75 }));

    const result = prepareLibraryPathForPython(configDir, 'D:\\Songs');

    assert.equal(result.status, 'bootstrapped');
    assert.deepEqual(
        JSON.parse(fs.readFileSync(configFile, 'utf8')),
        { master_difficulty: 75, dlc_dir: 'D:\\Songs' },
    );
});

test('an existing saved library stays config-owned and can change between scans', () => {
    const configDir = tmpConfigDir();
    const configFile = path.join(configDir, 'config.json');
    const savedSongs = path.join(configDir, 'Saved Songs');
    fs.mkdirSync(savedSongs);
    fs.writeFileSync(configFile, JSON.stringify({ dlc_dir: savedSongs }));

    const result = prepareLibraryPathForPython(configDir, 'C:\\Default Songs');

    assert.deepEqual(result, { status: 'configured' });
    assert.deepEqual(
        JSON.parse(fs.readFileSync(configFile, 'utf8')),
        { dlc_dir: savedSongs },
    );
    assert.equal(result.environmentDlcDir, undefined);
});

test('a missing saved library is reported without overwriting the intended path', () => {
    const configDir = tmpConfigDir();
    const configFile = path.join(configDir, 'config.json');
    const missingSongs = path.join(configDir, 'Disconnected Songs');
    fs.writeFileSync(configFile, JSON.stringify({ dlc_dir: missingSongs }));

    const result = prepareLibraryPathForPython(configDir, 'C:\\Default Songs');

    assert.equal(result.status, 'invalid-config');
    assert.match(result.error, /not an existing directory/);
    assert.deepEqual(JSON.parse(fs.readFileSync(configFile, 'utf8')), { dlc_dir: missingSongs });
});

test('a saved library file is rejected without overwriting the configured value', () => {
    const configDir = tmpConfigDir();
    const configFile = path.join(configDir, 'config.json');
    const file = path.join(configDir, 'not-a-library');
    fs.writeFileSync(file, 'x');
    fs.writeFileSync(configFile, JSON.stringify({ dlc_dir: file }));

    const result = prepareLibraryPathForPython(configDir, 'C:\\Default Songs');

    assert.equal(result.status, 'invalid-config');
    assert.match(result.error, /not an existing directory/);
    assert.deepEqual(JSON.parse(fs.readFileSync(configFile, 'utf8')), { dlc_dir: file });
});

test('an explicit valid DLC_DIR remains an environment override', () => {
    const configDir = tmpConfigDir();
    const managedSongs = path.join(configDir, 'Managed Songs');
    fs.mkdirSync(managedSongs);
    const result = prepareLibraryPathForPython(
        configDir,
        'C:\\Default Songs',
        ` ${managedSongs} `,
    );

    assert.deepEqual(result, {
        status: 'explicit-override',
        environmentDlcDir: managedSongs,
    });
    assert.equal(fs.existsSync(path.join(configDir, 'config.json')), false);
});

test('an explicit DLC_DIR rejects whitespace, files, and missing paths', () => {
    const root = tmpConfigDir();
    const file = path.join(root, 'not-a-directory');
    fs.writeFileSync(file, 'x');

    assert.equal(normalizeExistingLibraryDirectory('   '), undefined);
    assert.equal(normalizeExistingLibraryDirectory(file), undefined);
    assert.equal(normalizeExistingLibraryDirectory(path.join(root, 'missing')), undefined);
});

test('an explicit DLC_DIR is trimmed before directory validation', () => {
    const root = tmpConfigDir();
    const directory = path.join(root, 'Managed Songs');
    fs.mkdirSync(directory);

    assert.equal(normalizeExistingLibraryDirectory(`  ${directory}  `), directory);
});

test('a corrupt config is never overwritten during bootstrap', () => {
    const configDir = tmpConfigDir();
    const configFile = path.join(configDir, 'config.json');
    fs.writeFileSync(configFile, '{broken');

    const result = prepareLibraryPathForPython(configDir, 'C:\\Default Songs');

    assert.equal(result.status, 'invalid-config');
    assert.match(result.error, /JSON/);
    assert.equal(fs.readFileSync(configFile, 'utf8'), '{broken');
});

test('invalid-config exports the resolved fallback so the backend keeps a library', () => {
    // The backend has no built-in default (_get_dlc_dir returns None): with
    // the env var deleted AND config.json unusable, scans and starter
    // seeding would silently skip. Failure statuses must keep the pre-#117
    // env fallback.
    const configDir = tmpConfigDir();
    const configFile = path.join(configDir, 'config.json');
    fs.writeFileSync(configFile, JSON.stringify({ dlc_dir: path.join(configDir, 'gone') }));

    const result = prepareLibraryPathForPython(configDir, 'C:\\Default Songs');

    assert.equal(result.status, 'invalid-config');
    assert.equal(result.environmentDlcDir, 'C:\\Default Songs');

    const environment = {};
    applyLibraryPathToPythonEnvironment(environment, result);
    assert.deepEqual(environment, { DLC_DIR: 'C:\\Default Songs' });
});

test('corrupt config exports the resolved fallback without being overwritten', () => {
    const configDir = tmpConfigDir();
    const configFile = path.join(configDir, 'config.json');
    fs.writeFileSync(configFile, '{broken');

    const result = prepareLibraryPathForPython(configDir, 'C:\\Default Songs');

    assert.equal(result.status, 'invalid-config');
    assert.equal(result.environmentDlcDir, 'C:\\Default Songs');
    assert.equal(fs.readFileSync(configFile, 'utf8'), '{broken');
});

test('write-failed exports the resolved fallback so the backend keeps a library', () => {
    // Force the config write to fail by making config.json's parent a file.
    const root = tmpConfigDir();
    const configDir = path.join(root, 'not-a-dir');
    fs.writeFileSync(configDir, 'x');

    const result = prepareLibraryPathForPython(configDir, 'C:\\Default Songs');

    assert.equal(result.status, 'write-failed');
    assert.equal(result.environmentDlcDir, 'C:\\Default Songs');
});

test('Python environment omits DLC_DIR when config owns the library path', () => {
    const environment = { DLC_DIR: 'C:\\Stale Parent Value', KEEP: 'yes' };

    applyLibraryPathToPythonEnvironment(environment, { status: 'configured' });

    assert.deepEqual(environment, { KEEP: 'yes' });
});

test('Python environment exports only a validated explicit DLC_DIR', () => {
    const configDir = tmpConfigDir();
    const managedSongs = path.join(configDir, 'Managed Songs');
    fs.mkdirSync(managedSongs);
    const preparation = prepareLibraryPathForPython(
        configDir,
        'C:\\Default Songs',
        ` ${managedSongs} `,
    );
    const environment = { DLC_DIR: 'C:\\Stale Parent Value', KEEP: 'yes' };

    applyLibraryPathToPythonEnvironment(environment, preparation);

    assert.deepEqual(environment, { DLC_DIR: managedSongs, KEEP: 'yes' });
});

test('Python environment removes an invalid explicit DLC_DIR', () => {
    const configDir = tmpConfigDir();
    const fallback = path.join(configDir, 'Fallback Songs');
    fs.mkdirSync(fallback);
    const file = path.join(configDir, 'not-a-directory');
    fs.writeFileSync(file, 'x');
    const preparation = prepareLibraryPathForPython(configDir, fallback, file);
    const environment = { DLC_DIR: file };

    applyLibraryPathToPythonEnvironment(environment, preparation);

    assert.deepEqual(environment, {});
});
