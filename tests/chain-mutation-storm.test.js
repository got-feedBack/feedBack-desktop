// Phase 0.b storm test (docs/audio-engine-tlc.md §4, deep-read §1): chain-
// mutating async workers (loadPreset/loadVST/loadNAM/loadIR) queue on the
// libuv threadpool with no mutual exclusion, so two overlapping loadPreset
// calls can interleave clear()/addProcessor() and merge both presets into
// garbage. This test documents that corruption today and flips to a hard gate
// once ChainOps lands the chain-mutation serializer (plan phase 7).
//
// EXPECTED-FAIL / QUARANTINED: needs the built addon, drives a known race
// repeatedly (a single clean run proves nothing for a race), and initializes
// JUCE in-process. Run explicitly with:
//   CHAIN_STORM=1 node --test tests/chain-mutation-storm.test.js
'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const ADDON = path.join(__dirname, '..', 'build', 'Release', 'slopsmith_audio.node');
const ENABLED = process.env.CHAIN_STORM === '1';
const ITERATIONS = 50;

// Minimal valid 16-bit PCM mono WAV (a short impulse) — enough for IRLoader,
// so the presets need no real cab/amp assets. IR slots (type 2) are the only
// asset-cheap distinguishable payload loadPreset accepts.
function writeImpulseWav(file, numSamples) {
    const dataBytes = numSamples * 2;
    const buf = Buffer.alloc(44 + dataBytes);
    buf.write('RIFF', 0); buf.writeUInt32LE(36 + dataBytes, 4); buf.write('WAVE', 8);
    buf.write('fmt ', 12); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20);
    buf.writeUInt16LE(1, 22); buf.writeUInt32LE(48000, 24); buf.writeUInt32LE(96000, 28);
    buf.writeUInt16LE(2, 32); buf.writeUInt16LE(16, 34);
    buf.write('data', 36); buf.writeUInt32LE(dataBytes, 40);
    buf.writeInt16LE(32767, 44); // unit impulse, remaining samples zero
    fs.writeFileSync(file, buf);
}

const IR_TYPE = 2; // ProcessorSlot::Type::IR

function irPreset(irFile, slotCount) {
    return JSON.stringify({
        chain: Array.from({ length: slotCount }, (_, i) => ({
            type: IR_TYPE, name: `storm-ir-${slotCount}-${i}`, path: irFile, bypassed: false,
        })),
    });
}

test('concurrent loadPreset calls end with exactly one caller\'s chain', { skip: !ENABLED && 'needs built addon — set CHAIN_STORM=1 (hard gate since the phase-7 serializer)' }, async () => {
    assert.ok(fs.existsSync(ADDON), 'addon must be built (npm run build:audio)');
    const audio = require(ADDON);
    audio.init(); // returns undefined; loadPreset fails "No engine" if it didn't take

    const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'chain-storm-'));
    const irFile = path.join(tmp, 'impulse.wav');
    writeImpulseWav(irFile, 64);

    // Preset A loads 1 IR slot, preset B loads 2 — after both settle the chain
    // must be exactly one of those (1 or 2 slots of the SAME preset's names).
    // An interleaved clear/add merge shows up as 3 slots or mixed names.
    const presetA = irPreset(irFile, 1);
    const presetB = irPreset(irFile, 2);

    try {
        for (let i = 0; i < ITERATIONS; i++) {
            const [ra, rb] = await Promise.all([
                audio.loadPreset(presetA),
                audio.loadPreset(presetB),
            ]);
            assert.ok(ra?.success && rb?.success, `iteration ${i}: a load reported failure`);

            const slots = audio.getChainState();
            const names = slots.map((s) => s.name);
            const isA = names.length === 1 && names[0] === 'storm-ir-1-0';
            const isB = names.length === 2 && names[0] === 'storm-ir-2-0' && names[1] === 'storm-ir-2-1';
            assert.ok(isA || isB, `iteration ${i}: merged/corrupt chain: ${JSON.stringify(names)}`);
        }
    } finally {
        await audio.clearChain?.();
        audio.shutdown?.();
        fs.rmSync(tmp, { recursive: true, force: true });
    }
});
