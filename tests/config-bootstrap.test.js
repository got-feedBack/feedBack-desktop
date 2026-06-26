const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { loadTs } = require('./_load-ts');

// config-bootstrap imports `{ app } from 'electron'`, which in plain node resolves
// to the electron path string (no throw); migrateUserData never touches it.
const {
    migrateUserData,
    schedulePendingDeletion,
    consumePendingReset,
} = loadTs('src/main/config-bootstrap.ts');

function tmpAppData() {
    return fs.mkdtempSync(path.join(os.tmpdir(), 'feedback-bootstrap-'));
}

test('migrates a legacy userData folder into the new one when the new dir is missing', () => {
    const parent = tmpAppData();
    const legacy = path.join(parent, 'slopsmith-desktop');
    fs.mkdirSync(path.join(legacy, 'slopsmith-config'), { recursive: true });
    fs.writeFileSync(path.join(legacy, 'slopsmith-config', 'config.json'), '{"dlc_dir":"/x"}');
    fs.writeFileSync(path.join(legacy, 'slopsmith-desktop.json'), '{"lanAccess":false}');

    const newUserData = path.join(parent, 'feedback-desktop');
    const res = migrateUserData(newUserData, '2026-06-26T00:00:00.000Z');

    assert.equal(res.migrated, true);
    assert.equal(res.from, legacy);
    assert.ok(fs.existsSync(path.join(newUserData, 'slopsmith-desktop.json')), 'top-level file copied');
    assert.ok(fs.existsSync(path.join(newUserData, 'slopsmith-config', 'config.json')), 'nested file copied');
    assert.ok(fs.existsSync(path.join(newUserData, 'userdata-migrated.json')), 'migration marker written');
    // Legacy left in place as a fallback (copy, not move).
    assert.ok(fs.existsSync(path.join(legacy, 'slopsmith-desktop.json')), 'legacy preserved');
});

test('does NOT overwrite when the new userData dir already exists', () => {
    const parent = tmpAppData();
    const legacy = path.join(parent, 'fee[dB]ack');
    fs.mkdirSync(legacy, { recursive: true });
    fs.writeFileSync(path.join(legacy, 'old.json'), 'legacy');

    const newUserData = path.join(parent, 'feedback-desktop');
    fs.mkdirSync(newUserData, { recursive: true });
    fs.writeFileSync(path.join(newUserData, 'keep.json'), 'mine');

    const res = migrateUserData(newUserData, '2026-06-26T00:00:00.000Z');

    assert.equal(res.migrated, false);
    assert.match(res.reason, /already exists/);
    assert.ok(fs.existsSync(path.join(newUserData, 'keep.json')), 'existing data untouched');
    assert.ok(!fs.existsSync(path.join(newUserData, 'old.json')), 'legacy not copied over existing dir');
});

test('no-op when there is no legacy folder to migrate from', () => {
    const parent = tmpAppData();
    const newUserData = path.join(parent, 'feedback-desktop');
    const res = migrateUserData(newUserData, '2026-06-26T00:00:00.000Z');
    assert.equal(res.migrated, false);
    assert.match(res.reason, /no legacy/);
    assert.ok(!fs.existsSync(newUserData), 'new dir not created when nothing to migrate');
});

test('schedulePendingDeletion + consumePendingReset defer and then apply deletions', () => {
    const userData = tmpAppData();
    const held1 = path.join(userData, 'Local Storage');
    const held2 = path.join(userData, 'Crashpad');
    fs.mkdirSync(held1, { recursive: true });
    fs.mkdirSync(held2, { recursive: true });
    fs.writeFileSync(path.join(held1, 'leveldb'), 'x');

    assert.equal(schedulePendingDeletion(userData, [held1]), true);
    assert.equal(schedulePendingDeletion(userData, [held2, held1]), true); // merge + de-dup
    assert.equal(schedulePendingDeletion(userData, []), true); // nothing to do
    const manifest = path.join(userData, 'pending-reset.json');
    assert.deepEqual(JSON.parse(fs.readFileSync(manifest, 'utf8')).sort(), [held1, held2].sort());

    // Nothing deleted yet — deletion is deferred to next launch.
    assert.ok(fs.existsSync(held1));

    const applied = consumePendingReset(userData);
    assert.deepEqual(applied.sort(), [held1, held2].sort());
    assert.ok(!fs.existsSync(held1), 'deferred path removed on consume');
    assert.ok(!fs.existsSync(held2), 'deferred path removed on consume');
    assert.ok(!fs.existsSync(manifest), 'manifest cleared after consume');

    // Idempotent: a second consume with no manifest is a no-op.
    assert.deepEqual(consumePendingReset(userData), []);
});
