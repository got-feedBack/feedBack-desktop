const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { loadTs } = require('./_load-ts');

const {
    runConfigMigrations,
    readSchemaVersion,
    CURRENT_SCHEMA_VERSION,
} = loadTs('src/main/config-migrations.ts');

function tmpConfigDir() {
    return fs.mkdtempSync(path.join(os.tmpdir(), 'feedback-cfgmig-'));
}

test('first run stamps the config to CURRENT_SCHEMA_VERSION and is idempotent', () => {
    const dir = tmpConfigDir();
    assert.equal(readSchemaVersion(dir), 0);

    const first = runConfigMigrations(dir, '1.2.3', '2026-06-26T00:00:00.000Z');
    assert.equal(first.from, 0);
    assert.equal(first.to, CURRENT_SCHEMA_VERSION);
    assert.equal(readSchemaVersion(dir), CURRENT_SCHEMA_VERSION);

    const stamp = JSON.parse(fs.readFileSync(path.join(dir, 'config_version.json'), 'utf8'));
    assert.equal(stamp.schemaVersion, CURRENT_SCHEMA_VERSION);
    assert.equal(stamp.appVersion, '1.2.3');
    assert.equal(stamp.updatedAt, '2026-06-26T00:00:00.000Z');

    // Second run is a no-op: nothing to migrate, stamp unchanged.
    const second = runConfigMigrations(dir, '1.2.3', '2026-06-26T01:00:00.000Z');
    assert.equal(second.from, CURRENT_SCHEMA_VERSION);
    assert.equal(second.to, CURRENT_SCHEMA_VERSION);
    assert.deepEqual(second.ran, []);
});

test('fail-soft: a throwing migration is logged and skipped; later migrations still run', () => {
    const dir = tmpConfigDir();
    const okMarker = path.join(dir, 'ok-ran.txt');

    // Two version-1 migrations (both in range for a fresh dir). The first throws;
    // the runner must not abort — the second must still execute.
    const registry = [
        { version: 1, name: 'boom', run: () => { throw new Error('boom'); } },
        { version: 1, name: 'ok', run: () => fs.writeFileSync(okMarker, 'ran') },
    ];

    const res = runConfigMigrations(dir, '9.9.9', '2026-06-26T00:00:00.000Z', registry);

    assert.equal(res.ran.length, 2);
    assert.equal(res.ran[0].ok, false);
    assert.match(res.ran[0].error, /boom/);
    assert.equal(res.ran[1].ok, true);
    assert.ok(fs.existsSync(okMarker), 'the second migration ran despite the first throwing');
    // The stamp still advances so a persistently-failing migration cannot wedge startup.
    assert.equal(readSchemaVersion(dir), CURRENT_SCHEMA_VERSION);
});

test('readSchemaVersion treats a missing/corrupt stamp as version 0', () => {
    const dir = tmpConfigDir();
    assert.equal(readSchemaVersion(dir), 0);
    fs.writeFileSync(path.join(dir, 'config_version.json'), 'not json {');
    assert.equal(readSchemaVersion(dir), 0);
});
