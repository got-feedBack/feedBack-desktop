// Pins the JUCE WASAPI device-type names the shared player bundle keys on.
//
// The renderer decides "output is exclusive-style" by string-matching
// getCurrentDevice().outputType against 'Windows Audio (Exclusive Mode)'
// (see feedBack/static/app.js, _isExclusiveOutputType). That name is a
// hardcoded, unlocalised string inside the vendored JUCE. If a JUCE upgrade
// renames it, the feedpak-under-exclusive routing silently stops engaging —
// this test turns that into a build-time failure instead.

const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const WASAPI_CPP = path.join(
    __dirname, '..', 'JUCE', 'modules', 'juce_audio_devices', 'native',
    'juce_WASAPI_windows.cpp');

test('vendored JUCE still names the exclusive WASAPI type "Windows Audio (Exclusive Mode)"', () => {
    const src = fs.readFileSync(WASAPI_CPP, 'utf8');
    assert.ok(
        src.includes('return "Windows Audio (Exclusive Mode)"'),
        'JUCE WASAPI exclusive type name changed — update _isExclusiveOutputType in feedBack/static/app.js');
    // The shared-mode low-latency type must remain distinct: the renderer
    // predicate relies on exact equality so this name must NOT collapse into
    // the exclusive one.
    assert.ok(
        src.includes('"Windows Audio (Low Latency Mode)"'),
        'JUCE WASAPI low-latency type name changed — re-verify the exclusive predicate');
});
