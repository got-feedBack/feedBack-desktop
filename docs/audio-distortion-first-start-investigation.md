# Investigation: Heavy distortion on first start of mic monitoring (Rig Builder / NAM Tone Engine / Audio page)

**Date:** 2026-07-08
**Status:** Root-cause hypothesis identified (code-level, not yet reproduced with instrumentation)

## Symptom

- USB microphone, device type "Windows Audio" (shared or exclusive) or WASAPI variants.
- First time monitoring starts (opening Rig Builder, starting the NAM Tone Engine test in settings, first engine start on the Audio page), the monitored signal is **heavily distorted / garbled** — sounds like a sample-rate ("baud rate") mismatch.
- The distortion **persists** until one of these actions, after which audio is clean:
  1. Rig Builder: re-selecting "No Tone" (even if it was already "No Tone").
  2. NAM Tone Engine settings: stopping and restarting.
  3. Audio page: toggling "Use in-app amp sims" off/on.

## Common denominator of all three "fixes"

None of the fixes touch the audio *device*. All three tear down or rebuild the **SignalChain** (tone chain):

| Fix action | Code path | Effect |
|---|---|---|
| "No Tone" re-select | `clearChain` → `SignalChain::clear()` | processors destroyed |
| Amp-sims toggle | `src/renderer/screen.js:1642` → `api.clearChain()` + preset reload | processors destroyed + re-created, `prepareToPlay` re-run |
| Stop/restart | `stopAudio`/`startAudio` → `audioDeviceAboutToStart` → `SourceChain::prepare` → `SignalChain::prepare` (`SourceChain.cpp:33`) | `releaseResources` + `prepareToPlay` re-run on every slot |

Every fix ends in `prepareToPlay()` (and for NAM, `model->Reset()`) being re-run on the chain processors. So the corruption lives **inside the chain processors' DSP state**, not in the device or the split-mode ring.

## Prime suspect: NAM core buffer overrun when a callback block exceeds the prepared block size

### The unguarded invariant

The vendored NeuralAmpModelerCore pre-allocates all internal buffers to `maxBufferSize` at `Reset()` time:

- `src/audio/third_party/NAM/NAM/conv1d.cpp:121-140` — `SetMaxBufferSize` sizes `_input_buffer` (ring) and `_output` to `maxBufferSize`.
- `src/audio/third_party/NAM/NAM/dsp.cpp:469` — the only protection against a larger block is `assert(num_frames <= _output.cols())`, **a no-op in release builds**.

`NAMProcessor::processBlock` (`src/audio/NAMProcessor.cpp:104`) passes the JUCE callback's `numSamples` straight into `model->process(...)` with no clamp or chunking. If `numSamples > maxBufferSize` from the last `Reset()`:

- Eigen `leftCols(num_frames)` reads/writes past the allocated columns → garbage output.
- The Conv1D **ring buffer write position is corrupted / misaligned**, so the damage is **persistent**: every subsequent block (even correctly sized ones) is processed against a mangled ring → continuous heavy garbling until the next `Reset()`.

That persistence is exactly the observed behavior: distortion continues indefinitely and only a chain rebuild / re-prepare (all three "fixes") clears it, because each ends in `model->Reset()` via `NAMProcessor::prepareToPlay` (`NAMProcessor.cpp:55-66`).

### How an oversized block reaches the chain on Windows Audio / WASAPI

Two independent holes:

**(a) Duplex path has no block-size clamp.**
`AudioEngine::audioDeviceIOCallbackWithContext` (`src/audio/AudioEngine.cpp:2214-2232`): the split path clamps `numSamples` to the pre-sized scratch, but the **duplex** path wraps the device's `outputData` at the full delivered `numSamples` and runs the whole source chain (including `SignalChain::process`) on it. JUCE's WASAPI shared-mode device is known to deliver **oversized/accumulated blocks on the first callback(s) after a start or reconfigure** (and whenever its internal FIFO catches up). One oversized block > prepared `blockSize` is enough to permanently corrupt the NAM ring state (see above). Windows Audio shared mode is exactly the configuration the user reports; ASIO (fixed block sizes) would not hit this — consistent with the report.

**(b) Stale prepare race when the chain is (re)built during device configuration.**
`SignalChain::addProcessor` (`src/audio/SignalChain.cpp:399-426`) prepares the incoming processor at the chain's *members* `currentSampleRate`/`currentBlockSize` (defaults 48000/256, `SignalChain.h:130-131`). Chain loads run on N-API background workers (`LoadNAMWorker`/`LoadIRWorker`/preset workers in `src/audio/NodeAddon.cpp`) concurrently with the renderer's `setDevice`/`startAudio` sequence. A processor added *after* the last `SignalChain::prepare()` but prepared from a *pre-reconfigure* snapshot keeps the wrong `blockSize` (e.g. prepared at 256 while WASAPI shared actually delivers 441/448/480-sample blocks) — and **nothing re-prepares it** until the next device start or chain rebuild. First-open timing makes this window easy to hit exactly once, matching "first time only".

Note the two holes compound: (b) makes `maxBufferSize` too small; (a) lets the too-large block through to trigger the NAM overrun.

### Why it also shows up with "No Tone" selected

At app init, `screen.js` auto-loads the default preset / saved chain into the engine when amp sims are enabled (`src/renderer/screen.js:986-1000`). The engine can therefore hold live NAM/IR processors even while the Rig Builder UI shows "No Tone". Re-selecting "No Tone" issues an actual `clearChain`, destroying the corrupted processors — hence "resetting it to No Tone again fixes it".

## Secondary suspects considered and mostly ruled out

- **True sample-rate mismatch in the chain** (NAM `Reset` at 48 k, device at 44.1 k): possible via race (b), but alone it causes a tonal/pitch shift, not persistent heavy garbling; kept as a contributing factor.
- **Split-mode output ring (`packStereoIntoRing` / `audioOutputCallback`)**: has self-correcting catch-up/underflow branches (`AudioEngine.cpp:2820-2872`); a chain rebuild would not fix a ring problem. Ruled out as the persistent cause.
- **IRLoader / juce::dsp::Convolution**: JUCE convolution resamples the IR on `prepare()` and tolerates block-size changes; self-healing. Ruled out.
- **Input/output devices opened at different rates in split mode**: explicitly rejected with an error (`AudioEngine.cpp:1126-1131`). Ruled out.

## Recommended fixes (defense in depth)

1. **Chunk in `NAMProcessor::processBlock`** — process `numSamples` in slices of at most the prepared `currentBlockSize` (no allocation needed; loop over the existing mono buffers). This alone removes the memory corruption regardless of who delivers an oversized block.
2. **Clamp/chunk in `SignalChain::process`** — if `buffer.getNumSamples() > currentBlockSize`, process in `currentBlockSize` slices so *every* slot (VST, NAM, IR) only ever sees blocks it was prepared for. (VST3s have the same maxBlockSize contract; today they're equally exposed on the duplex path.)
3. **Prepare with headroom** — prepare the chain at e.g. `2 × device blockSize` to absorb WASAPI's first-callback burst behavior cheaply.
4. **Close race (b)** — stamp a device-config generation counter in `AudioEngine`; `addProcessor`/`replaceProcessor` records the generation it prepared against, and `audioDeviceAboutToStart` (or a post-`setAudioDevices` pass) re-prepares any slot with a stale stamp. Alternatively: after `setAudioDevices` completes, unconditionally re-run `SignalChain::prepare` once the device values are final (it already is idempotent).
5. Optional hardening upstream: replace the `assert` at `dsp.cpp:469` with a real clamp/early-return so a future caller can never corrupt state silently.

## How to confirm before fixing

The engine already logs to stderr (`[AudioEngine] Actual device setup: sr=… bs=…`). Add two temporary logs:

- In `audioDeviceIOCallbackWithContext` (duplex branch): warn when `numSamples != inputBlockSize` (rate-limited) — expected to fire on the very first callback(s) after opening the WASAPI device.
- In `SignalChain::addProcessor`: log the `sr`/`bs` each processor is prepared with, plus a timestamp — compare against the device-setup log ordering to confirm race (b).

Reproduce: USB mic, Windows Audio (shared), open Rig Builder for the first time in a session with a NAM-based tone loaded. Expect: oversized-first-block warning, then persistent distortion; re-select "No Tone" → clean.

## Key file/line references

- `src/audio/NAMProcessor.cpp:55-66, 104` — Reset on prepare; unclamped `process`.
- `src/audio/third_party/NAM/NAM/dsp.cpp:93-113, 469` — `maxBufferSize` plumbing; release-mode no-op assert.
- `src/audio/third_party/NAM/NAM/conv1d.cpp:121-145` — fixed-size ring/output buffers.
- `src/audio/AudioEngine.cpp:2214-2232` — duplex path lacks the split path's block clamp.
- `src/audio/SignalChain.cpp:217-239, 399-426` — prepare vs. addProcessor stale-snapshot race.
- `src/audio/SourceChain.cpp:14-39` — device-start re-prepare (why restart fixes it).
- `src/renderer/screen.js:986-1000, 1642-1674` — auto-loaded chain behind "No Tone"; amp-sims toggle = chain rebuild.
