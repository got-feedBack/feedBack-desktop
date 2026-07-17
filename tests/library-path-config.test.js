'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { loadTs, ROOT } = require('./_load-ts');

const {
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
    fs.writeFileSync(configFile, JSON.stringify({ dlc_dir: 'D:\\Saved Songs' }));

    const result = prepareLibraryPathForPython(configDir, 'C:\\Default Songs');

    assert.deepEqual(result, { status: 'configured' });
    assert.deepEqual(
        JSON.parse(fs.readFileSync(configFile, 'utf8')),
        { dlc_dir: 'D:\\Saved Songs' },
    );
    assert.equal(result.environmentDlcDir, undefined);
});

test('an explicit valid DLC_DIR remains an environment override', () => {
    const configDir = tmpConfigDir();
    const result = prepareLibraryPathForPython(
        configDir,
        'C:\\Default Songs',
        ' D:\\Managed Songs ',
    );

    assert.deepEqual(result, {
        status: 'explicit-override',
        environmentDlcDir: 'D:\\Managed Songs',
    });
    assert.equal(fs.existsSync(path.join(configDir, 'config.json')), false);
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

test('python startup does not pin its resolved fallback as DLC_DIR', () => {
    const source = fs.readFileSync(path.join(ROOT, 'src', 'main', 'python.ts'), 'utf8');

    assert.match(source, /prepareLibraryPathForPython\(configDir, dlcDir, explicitDlcDir\)/);
    assert.doesNotMatch(source, /DLC_DIR:\s*dlcDir/);
    assert.match(source, /delete pythonEnv\.DLC_DIR/);
});
