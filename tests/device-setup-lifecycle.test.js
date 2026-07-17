'use strict';

// Source-level lifecycle contract for the hardware-dependent half of the ASIO
// repair. The pure format decision table is covered by rate_match_test.cpp;
// these assertions pin the ordering that cannot be exercised without loading
// a real Windows ASIO driver in CI.

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const source = fs.readFileSync(
    path.join(__dirname, '..', 'src', 'audio', 'engine', 'DeviceSetup.cpp'),
    'utf8').replace(/\r\n/g, '\n');

const start = source.indexOf('juce::String DeviceSetup::applyDuplex');
const end = source.indexOf('DeviceConfigResult DeviceSetup::applySplit', start);
// Fail with a clear message before any slicing if the function markers move —
// a bad slice would otherwise make every assertion below fail confusingly.
assert.ok(start >= 0 && end > start, 'could not locate applyDuplex in DeviceSetup.cpp');
const applyDuplex = source.slice(start, end);

test('duplex setup closes Windows ASIO before constructing its channel probe', () => {
    assert.match(
        applyDuplex,
        /#elif JUCE_WINDOWS\s+closeBeforeReconfigure = \(currentTypeName == "ASIO"\);/);

    const guardedClose = applyDuplex.indexOf(
        'if (closeBeforeReconfigure && inMgr.getCurrentAudioDevice() != nullptr)');
    const close = applyDuplex.indexOf('inMgr.closeAudioDevice();', guardedClose);
    const probe = applyDuplex.indexOf('type->createDevice(outputName, inputName)');
    assert.ok(guardedClose >= 0 && close > guardedClose && probe > close,
        'the live ASIO device must be closed before a temporary probe is created');
});

test('duplex setup never converts requested-device failure into default-device success', () => {
    assert.doesNotMatch(applyDuplex, /initialiseWithDefaultDevices/);
    assert.match(applyDuplex, /return failClosed\("device setup failed: " \+ result\);/);
});

test('duplex success is gated on actual rate, buffer, and active channel masks', () => {
    assert.match(applyDuplex, /getCurrentSampleRate\(\)/);
    assert.match(applyDuplex, /getCurrentBufferSizeSamples\(\)/);
    assert.match(applyDuplex, /getActiveInputChannels\(\)/);
    assert.match(applyDuplex, /getActiveOutputChannels\(\)/);
    assert.match(applyDuplex, /validateOpenedDeviceFormat\(/);
});

test('duplex failures clear observable format state and release monitor resources', () => {
    const cleanupStart = applyDuplex.indexOf('auto failClosed =');
    const cleanupEnd = applyDuplex.indexOf('// Channel masks must match too', cleanupStart);
    const cleanup = applyDuplex.slice(cleanupStart, cleanupEnd);

    assert.match(cleanup, /inMgr\.closeAudioDevice\(\)/);
    assert.match(cleanup, /currentSampleRate\.store\(0\.0/);
    assert.match(cleanup, /inputBlockSize\.store\(0/);
    assert.match(cleanup, /outputBlockSize\.store\(0/);
    assert.match(cleanup, /monitorChain\.releaseMonitorChain\(\)/);
});
