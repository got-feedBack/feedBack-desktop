> **Execution status (2026-07-14, branch `refactor/audio-engine-tlc`):** implemented.
> Phases 0-8 of Part IV/V are complete on this branch, including the chain-mutation
> serializer + chainGeneration (native + executor), the monitor-mute arbiter, the
> single persistence store, and getLatencyBreakdown. Two sequencing changes vs. the
> roadmap below: the gain-sanitization fix shipped first (before phase 1), and the
> Part II ownership work that needs the rig_builder repo (single chain owner,
> legacy-path deletion, alias removal) is NOT on this branch. Drift note: the
> "21 clearChain call sites" count in Part II grew to 30 by execution time.

# Audio Engine TLC — Consolidated Findings & Refactor Plan

TLC pass on the feedBack-desktop audio engine (2026-07-12, branch `fix/loopback-capture-permission`).
Single consolidated document; supersedes the four separate docs
(overview / collisions / deep-read / refactor-plan).

Contents:
- **Part I** — how the engine works and integrates with the app, effects (NAM/VST/IR), plugins.
- **Part II** — redundant code, settings with multiple writers, control collisions.
- **Part III** — line-level deep read of `AudioEngine.cpp` and `NodeAddon.cpp`.
- **Part IV** — decomposition plan for the monolithic files.
- **Part V** — merged priority roadmap.

---

# Part I — Architecture Overview
Covers: engine architecture, desktop-app integration, input/output paths, effects (NAM / VST / IR),
detection pipeline, and which bundled plugins touch the engine and how.

---

### 1. Layer map

```
Renderer plugins (rig_builder, note_detect, stems, …)
        │  window.feedBackDesktop.audio.* / .audioEffects.*   (aliased: slopsmithDesktop)
        ▼
preload.ts  ── contextBridge, ~99 audio methods + audioEffects methods
        │  ipcRenderer.invoke / send
        ▼
Main process
  audio-bridge.ts          102 ipcMain.handle channels (audio:*, audio-effects:*)
  audio-effects-executor.ts  validates chain plans, drives native chain
  vst-crash-guard.ts       sentinel files → blocklist crashy VSTs across restarts
  plugin-manager.ts        git-based plugin install/update (server plugins)
        │  require('slopsmith_audio.node')
        ▼
NodeAddon.cpp (N-API, ~160 KB)  ── marshals JS ⇄ C++, async workers for VST loads
        ▼
AudioEngine (JUCE, C++)  src/audio/
  ├── SourceChain ×8 (pooled)  per-input capture + detect + tone chain
  │     └── SignalChain  ordered ProcessorSlots (VST | NAM | IR)
  ├── Backing-track transport + signalsmith-stretch
  ├── Stream sink (2nd output device for OBS/Discord)
  └── Renderer bus (WebAudio master → engine output)
        ▼
JUCE AudioDeviceManager(s)  → WASAPI / ASIO / DirectSound / CoreAudio / ALSA / JACK

Out-of-process: slopsmith-vst-host.exe (src/vst-host/main.cpp) — VST3 sandbox child.
```

Key files:

| Area | File |
|---|---|
| Engine core | `src/audio/AudioEngine.{h,cpp}` (47K header / 156K impl) |
| Per-input chain | `src/audio/SourceChain.{h,cpp}` |
| Effects chain | `src/audio/SignalChain.{h,cpp}` |
| NAM | `src/audio/NAMProcessor.{h,cpp}` (wraps NeuralAmpModelerCore, `src/audio/third_party/NAM`) |
| IR / cab | `src/audio/IRLoader.{h,cpp}` (juce::dsp::Convolution) |
| VST hosting | `src/audio/VSTHost.{h,cpp}` |
| VST sandbox | `src/audio/Sandbox/*` + `src/vst-host/main.cpp` |
| Detection | `PitchDetector` (YIN), `MlNoteDetector` (Basic Pitch ONNX), `ChordScorer`, `NoteVerifier`, `OnsetDetector` |
| Utility DSP | `NoiseGate`, `TonePolish`, `BackingLeveler`, `AudioSanitize` |
| JS bridge | `src/audio/NodeAddon.cpp`, `src/main/audio-bridge.ts`, `src/main/preload.ts` |

---

### 2. AudioEngine core

`AudioEngine` is a `juce::AudioIODeviceCallback` owning **two** `AudioDeviceManager`s:

- **Duplex mode** (default): `inputDeviceManager` owns both directions; one callback
  (`audioDeviceIOCallbackWithContext`) reads input, processes, writes output directly.
- **Split mode**: input-only on `inputDeviceManager`, output-only on `outputDeviceManager`
  (separate device types possible, e.g. ASIO in + WASAPI out). Processed stereo crosses via
  `outputPendingRing` — a lock-free SPSC ring of 4096 frames where each stereo frame is packed
  into one `atomic<uint64_t>` (bit_cast L|R) so reads are tear-free. ~85 ms of drift absorption.
  Input and output block sizes may differ; the ring absorbs asymmetry.

Device management surface: enumerate types/devices, dual-type probing
(`probeDeviceOptionsDual` — sample-rate intersection, `compatible` flag), `setAudioDevices`,
metrics (overflow/underflow counters, ring fill). Config persisted by the renderer via
`audio:saveDeviceSettings` / `loadDeviceSettings` (legacy single-`type` settings are mirrored to
input+output type).

Threading model (recurring pattern throughout): audio thread never locks — atomics everywhere,
lock-free SPSC rings, `static_assert(is_always_lock_free)`, control-thread mutation via
pending-flag handoff (e.g. `backingPendingSpeed`), and drop-oldest on overflow with counters.

---

### 3. Audio input

#### Sources (multi-input)

Per-input state lives on **SourceChain**, a fixed pool of `kMaxSources = 8` constructed up front
(pointers never reassigned → no race with audio thread; add/remove only flips an atomic `active`
flag). `sources[0]` is the permanent legacy default input; the engine facade forwards the
single-source API to it so NodeAddon/renderer needed no change.

- `addSource(inputChannel, deviceKey)` — bind another channel of the current device
  (multi-channel interfaces, e.g. Valeton GP-5) or of an **additional physical input device**
  (`bindInputDevice(deviceKey, name)`, up to 3 extras, each at its own clock; forces split mode).
- Removal uses a per-deviceKey `callbacksInFlight` counter handshake; wedged callbacks defer the
  release (`pendingRelease[]`) instead of blocking.
- Per-source: input gain, channel select (-1 = mono mix), monitor mute/kill, meters, verifier
  offsets (auto device-latency delta + user fine-tune, summed).

#### Per-source capture path (SourceChain::processBlock, audio thread)

```
device input → channel select / mono mix → inputGain
  ├─→ MlNoteDetector feed + pre-gate inputFrameRing (8192, SPSC)   [getInputFrame/getInputSince]
  → NoiseGate (post-gain, pre-FX; pitch detector sees ungated signal)
  ├─→ YIN PitchDetector feed + post-gate rawAudioRing (16384)      [getRawAudioFrame → tuner]
  → SignalChain (VST/NAM/IR tone chain)
  → sanitize (non-finite/runaway scrub, counted — issue #403)
  → monitor mute / monitor kill / chainOutputGain → TonePolish (fixed 3-band EQ, guitar bus only)
  → summed into output mix (sourceMonitorScratch, pre-sized)
```

Monitor semantics: `monitorMute` mutes dry pass-through only when chain empty (suppressible
around song-load chain rebuilds); `monitorKill` silences the guitar bus unconditionally
(external-rig users), applied to every pooled source.

---

### 4. Effects — SignalChain

Ordered `ProcessorSlot`s, each `Type::{VST, NAM, IR, Empty}` holding a
`unique_ptr<juce::AudioProcessor>`. Features:

- **Routing**: per-slot `pan` (constant-power), `branch` (0 = serial trunk, ≥1 = parallel branch,
  branches read the pre-split signal, panned outputs summed at merge), `branchSrc` (branch reads
  L / R / both), `postGain` (per-amp loudness trim). Pre-allocated scratch buffers; all-trunk path
  pays nothing.
- **State**: per-slot base64 VST state; whole-chain JSON preset save/load (`savePreset` /
  `loadPreset`); `replaceProcessor` swaps a slot in place (same id/position) — used for sandbox
  promotion and `replaceIR` cab swaps; `setSlotState` for the tone-switcher's incremental rebuild.
- **MIDI**: lock-free SPSC queue (64 msgs), `queueMidiMessage(slotId, msg)` from the N-API thread
  → drained on audio thread (`audio:sendMidiToSlot`).
- Oversized device blocks (WASAPI shared after device start) are sliced to the prepared block size.
- SEH/signal guards around plugin prepare/state calls; faults blocklist the plugin path.

#### Processor types

- **NAMProcessor** — mono in/out neural amp model (`.nam`), NeuralAmpModelerCore backend
  (`SLOPSMITH_NAM_SUPPORT`). Async-safe model load: staged `pendingModel`, atomic swap.
  Input/output level params. No editor.
- **IRLoader** — cab IR convolution (`.wav/.aif/.ir`) via `juce::dsp::Convolution`.
- **VST3** via **VSTHost**: background directory scanning (per-file subprocess probe through
  `slopsmith-vst-host --scan-plugin` → XML merge, so crashy plugins can't kill the app),
  known-plugin persistence, sync `loadPlugin` + async `loadPluginAsync` (message-thread pumping
  required for AmpliTube-class plugins that post messages to themselves during init).

#### VST sandbox (out-of-process)

`SandboxedProcessor` (src/audio/Sandbox) is a `juce::AudioProcessor` façade that forwards
everything to a spawned `slopsmith-vst-host.exe` child over a control pipe + shared-memory audio
channel (`Protocol.h`; platform impls `_win` / `_posix` / `_shared`). Properties:

- One child per plugin; child dies with the processor. Crash → `isAlive()` false → audio thread
  inserts silence; `CrashCallback` + `CrashAttribution` report which plugin died.
- Child owns the plugin editor as its own top-level window (Reaper-style; cross-process HWND
  reparenting broke D3D/GL plugins like Neural DSP Archetypes).
- **Promotion path**: an in-process VST3 is promoted to the sandbox when its editor is opened
  (in-process editors are the Windows WndProc/Qt crash path). `captureVstStateForPromotion`
  snapshots state under lock + SEH guard, then `replaceProcessor` swaps in the sandboxed twin.
- Child guarantees JUCE MessageManager on the OS main thread (impossible in the Node addon where
  V8 owns it); audio runs on a dedicated ring-drain worker.
- Known v1 gaps (documented in header): no `AudioProcessorParameter` proxies; bus layout
  hard-coded stereo↔stereo.
- **vst-crash-guard.ts** (main process): arms a sentinel file before risky load/editor operations;
  a crash leaves the sentinel behind → next launch blocklists that plugin.

---

### 5. Audio output

Three output paths, mixed in the engine:

1. **Monitor output** (primary device): sum of active sources' processed guitar buses
   + backing-track mix (`backingVolume` fader, `BackingLeveler` per-song loudness normalizer)
   + master `outputGain`. Duplex writes in-callback; split drains `outputPendingRing`.
2. **Stream sink** — an ADDITIONAL output device carrying an independent submix
   (backing/game and/or guitar monitor, own gain, sanitized 0..8) for OBS/Discord capture.
   `setStreamOutputDevice` / `setStreamBus`; underflow/overflow counters + level meter exposed.
3. **Renderer bus** (Phase 2, exclusive-mode support): the renderer's WebAudio master mix is
   pushed over IPC (`audio:pushRendererAudio`, fire-and-forget `ipcRenderer.send`) into a large
   SPSC ring (65536 frames ≈ 1.5 s; producer is the jittery IPC thread), linear-resampled
   producer-side to device rate, mixed into engine output. Keeps song/stem audio audible when the
   output device is exclusive-style (ASIO / WASAPI exclusive) and the OS mixer path is silent.
   Fed renderer-side by a whole-app `getDisplayMedia({audio})` loopback capture —
   `setDisplayMediaRequestHandler` in `main.ts` grants it (audio-only, own-app loopback; other
   apps' audio not captured). Current branch (`fix/loopback-capture-permission`) fixes the media
   permission handler to allow this getDisplayMedia request.

**Backing track**: JUCE `AudioFormatReaderSource` → `AudioTransportSource` buffered by a
`TimeSliceThread` read-ahead → optional signalsmith-stretch phase vocoder for speed change
(1x bypass path; lock-free speed handoff via pending atomic so slider drags never block the RT
tryLock). Playhead = accumulated heard frames minus stretcher latency; non-blocking cached
position/duration getters. Used for local audio-file playback; sloppak/HTML5-routed songs play
through the renderer's WebAudio instead (engine playhead frozen; the verifier is fed the
renderer's corrected playhead via `setPlayhead`).

---

### 6. Detection & scoring (engine-side)

- **PitchDetector** — monophonic YIN, sub-Hz parabolic interpolation, reads post-gate signal
  (silent when gate closed). Backs the always-on home tuner (`audio:getRawPitch`).
- **MlNoteDetector** — polyphonic Basic Pitch ONNX model (`loadNoteModel`); armed only while a
  consumer actually reads ML notes (`setMlNoteDetectionEnabled`) so ONNX inference isn't paid
  otherwise. `getActiveDetection()` prefers ML when loaded, else YIN — same shape either way.
- **ChordScorer** — scores a renderer-supplied chord context against the input ring
  (`audio:scoreChord`); ML-backed variant when the ML detector is live.
- **NoteVerifier** — background thread per source; renderer pushes the chart once (`setChart`),
  verifier scores each note's timing window against the live playhead + input ring, renderer
  drains verdicts (`getNoteVerdicts`). Replaced the per-tick scoreChord IPC loop that starved on
  dense passages. Playhead offset = auto device-latency delta + user fine-tune.
- **OnsetDetector** — consumes the input ring gaplessly via `getInputSince`.

---

### 7. Main-process integration

- **audio-bridge.ts** (65K): loads `slopsmith_audio.node`, registers all 102 `audio:*` /
  `audio-effects:*` IPC handlers, normalizes/persists device settings, wires vst-crash-guard
  sentinels around VST loads and editor opens, forwards renderer-bus audio.
- **audio-effects-executor.ts**: the capability-pipeline backend for the `audio-effects`
  capability. Accepts validated **chain plans** (`feedBack.audio_effects.chain_plan.v1`, legacy
  `slopsmith.…` schema accepted): up to 24 stages of kind `nam | ir | vst | utility | bypass`
  with roles (pedal/amp/cab/…), route keys (default `desktop-main`), gain sets, authorization
  gating (`user-action` / `restore-selection` / `playback-session`). Translates plans into native
  calls (loadPreset/clearChain/setBypass/setParameter/setGain/…) and reports structured outcomes
  (`handled | degraded | failed | unavailable | no-target | user-action-required`).
- **preload.ts**: exposes the whole surface as `window.feedBackDesktop` (alias
  `window.slopsmithDesktop`) — `audio.*` (~99 methods) + `audioEffects.*`.
- **NodeAddon.cpp**: N-API glue; libuv async workers for plugin loads (message thread keeps
  pumping); sandbox-aware VST loading (`loadVstSandboxAware`); shutdown cancels pending loads.

---

### 8. Bundled plugins that touch audio

Plugins are renderer-side (screen.js + plugin.json manifest, capability-pipelines.v1). The ones
interacting with the engine:

| Plugin | Interaction |
|---|---|
| **audio_engine** (bundled in this repo, `src/renderer/`) | The engine's own UI: device setup, chain editor, meters. Declares provider capabilities `audio-input`, `audio-mix`, `audio-monitoring`; observes `playback` lifecycle to rebuild tone automation / tear down native chain state. |
| **rig_builder** | Biggest consumer. Builds amp/cab/pedal rigs; declares `audio-effects` capability and submits chain plans (NAM stages, IRs, VSTs, per-stage gain/bypass/params) through audio-effects-executor. Also privileged-capabilities, jobs, library. Own repo also contains a VST (`vst/`) and tone-curation tooling. |
| **nam_tone** | NAM tone library (server-side): manages `nam_models/`, `nam_irs/`, `nam_tone.db` on the Python backend; models/IRs are what `audio:loadNAMModel` / `loadIR` consume. |
| **note_detect** | Real-time detection/scoring: arms ML detection, pushes charts (`setChart`), drains verdicts, reads pitch/raw frames, drives per-source scoring APIs. |
| **stems** / **stem_mixer** | WebAudio-side stem mix (`audio-mix` capability, mute/volume commands). Their master mix reaches the engine only via the renderer bus on exclusive-mode outputs. |
| **midi_amp** | Sends MIDI Program Change to external amps/modelers on tone switches (external gear path; engine-side per-slot MIDI exists via `audio:sendMidiToSlot`). |
| **tuner (built-in home tuner)** | Always-on YIN readout via `getRawPitch` / `getRawAudioFrame` — deliberately never pays ONNX cost. |
| **virtuoso / practice / minigames** | Consume detection results (verdicts/pitch) rather than driving the chain. |

Plugin lifecycle: `plugin-manager.ts` installs/updates plugins as git checkouts under the
plugins dir (https-only remotes, path-safe names); restart of the Python backend activates them.

---

### 9. Observations for the TLC pass (starting points)

- `AudioEngine.cpp` (156K) and `NodeAddon.cpp` (160K) are monoliths; SourceChain extraction
  ("Phase 0/2" comments) is mid-flight — multi-source fan-out phases still landing.
- Duplicated facade surface: engine forwards ~40 single-source methods to `source0()` while a
  parallel `getSource(id)`-indexed API grows alongside (`audio:*` vs `audio:setSource*`).
- Sandbox v1 gaps documented in `SandboxedProcessor.h` (no parameter proxies, fixed stereo buses).
- Three separate SPSC ring implementations (outputPendingRing, renderer bus, stream sink) share
  the packed-LR pattern — candidate for one templated ring.
- Backing-track transport is legacy for sloppak songs (renderer WebAudio does playback); the
  frozen-playhead special case leaks into NoteVerifier via `setPlayhead`.
- Naming drift: slopsmith → feedBack rebrand half-done (addon name `slopsmith_audio.node`,
  `slopsmith-vst-host.exe`, legacy schema ids, `window.slopsmithDesktop` alias).

---

# Part II — Redundancy & Control Collisions
Every finding below was
verified in source; file references point at the current tree.

Severity legend: 🔴 active conflict (two writers fight at runtime) · 🟠 dual ownership
(same setting settable from two places, last-writer-wins, no arbitration) · 🟡 redundancy
(duplicate surface/code, no runtime conflict yet).

---

### 1. 🔴 Signal chain has three independent writers

The native `SignalChain` is a single global resource, but three parties load/clear it:

1. **audio_engine bundle** (`src/renderer/screen.js`): direct `api.loadVST` / `loadNAMModel`
   / `loadIR` / `loadPreset` / `clearChain` (21 `clearChain` call sites), plus its own tone
   auto-switch/automation (`applyToneMappingsNow`, `applyToneAutomationFor`,
   `_restorePresetBlob` → `clearChain` + `loadPreset`).
2. **rig_builder via capability pipeline**: `audioEffects.loadPlan` → main-process
   `audio-effects-executor.ts` → `nativeAudio.loadPreset`.
3. **rig_builder legacy direct path**: `feedBackDesktop.audio.loadPreset` (tracked in its own
   telemetry as `audio-effects.legacy-native-load`).

Concrete evidence of the fight (rig_builder `screen.js`):

> "PROACTIVE TRANSIENT KILL: the bundle calls loadPreset ~1ms after we return this response.
> We can't monkey-patch `feedBackDesktop.audio.loadPreset` … the object is frozen by
> contextBridge"

rig_builder ships timing hacks (`_rbUnmuteTimer`, transient kill, fallback unmute) purely to
survive the bundle re-loading the chain right after it did. That is two plugins racing on the
same native chain with wall-clock heuristics as the arbiter.

**Additional executor-state hazard**: the executor keeps a `routes` map with
`stageSlots` (stageId → native slotId). Any direct `loadPreset` / `clearChain` /
`removeProcessor` / `moveProcessor` from path 1 or 3 invalidates those slot ids silently —
subsequent `setStageBypass` / `setStageParameter` / `activateSegment` then flip
bypass/params on the **wrong slots** (slot ids are reused sequentially by `nextSlotId`) or
return `no-target`. Nothing detects the divergence.

### 2. 🔴 Monitor mute: five writers, one atomic, persisted preference gets clobbered

`SourceChain::monitorMuted` writers:

| Writer | Where | When |
|---|---|---|
| audio_engine settings UI checkbox | `screen.js` (`ae-monitor-mute`) | user toggle; persisted in device settings |
| startup restore | `screen.js` ~901 | pushes saved value into engine on boot |
| executor preload-mute | `audio-effects-executor.ts:479-489` | saves `previousMonitorMute`, forces mute/unmute during chain load, restores on a `setTimeout` ramp |
| executor `releaseRoute` | `:616` | **unconditionally** `setMonitorMute(true)` + `setMonitorMuteSuppressed(false)` |
| renderer song-load suppression | `screen.js` (2 sites) + `audio:setMonitorMuteSuppressed` | temporary override around chain rebuild |

Collisions:

- `releaseRoute` forces mute=true regardless of the user's persisted `monitorMute:false`
  preference — the checkbox UI and the engine now disagree until the next toggle/restart.
- The executor's read-modify-restore (`previousMonitorMute` + delayed `schedulePreloadRestore`)
  races a user toggling the checkbox during the hold window: the restore overwrites the fresh
  user choice with the stale snapshot. `preloadRestoreVersion` guards against *newer executor
  loads*, not against other writers.
- `monitorMuteSuppressed` is set by both the renderer (song load) and executor flows with no
  refcount — whoever clears last wins; overlapping windows un-suppress early.

### 3. 🔴/🟠 Gain: four knobs, three surfaces, inconsistent clamping

Native gains: per-source `inputGain`, per-source `chainOutputGain`, global `outputGain`
(master), `backingVolume` — all reachable through `audio:setGain(which, value)`
(`NodeAddon.cpp SetGain`), and `input`/`chain` also through
`audio-effects:setRouteGain` + chain-plan `options.gains` + `preloadMute.targetGain`.

- **Dual ownership of `chain` gain**: audio_engine screen sets it (9 `setGain` sites);
  the executor zeroes it (`trySetGain('chain', 0)` on load and on `releaseRoute`) and later
  ramps it to `targetGain` (default **1**, or plan-supplied) on a timer. If the user (or tone
  automation) set chain gain meanwhile, the ramp silently overwrites it. Same
  stale-snapshot race as monitor mute.
- **Clamp inconsistency**: executor clamps to `0..32` (`clampGain`); `NodeAddon::SetGain` does
  **no** validation — `NaN`/`Infinity` from any direct `audio:setGain` caller reaches
  `outputGain.store()` / `inputGain.store()` raw. The engine sanitizes only the *stream* and
  *renderer-bus* gains (`sanitizeStreamGain`, 0..8, explicitly "so a NaN/Inf from JS can never
  reach the ring") — the exact same hazard is unguarded for master/input/chain/backing.
  A NaN master gain silences output and poisons the peak meters.
- Per-slot `postGain` overlaps conceptually with `chainOutputGain` (both are "level after the
  amp"): rig plans carry per-stage loudness trims while the screen's chain gain scales the
  same signal — two normalization layers, no documented ownership.

### 4. 🟠 Device settings: two persistence stores, newest-timestamp arbitration

`screen.js loadDeviceSettings()` merges **file-backed** settings (main process,
`audio:saveDeviceSettings`) with **`localStorage['slopsmith-audio-device']`**, picking
whichever has the newer `savedAt`. Two stores for one setting means:

- A main-side migration/reset (`config-reset.ts` territory) leaves stale localStorage that can
  win the timestamp race and resurrect wiped settings.
- `monitorMute` / `monitorKill` ride inside the *device* settings blob, so a device re-save
  from one path re-persists mute flags captured from checkbox state at that moment —
  interleaving with §2's runtime writers.
- Renderer keeps 10 `slopsmith-*` localStorage keys total (`slopsmith-signal-chain`,
  `slopsmith-chain-presets`, `slopsmith-tone-automation`, …) — the chain is *also* persisted
  renderer-side while rig_builder persists rigs server-side (`routes.py` / DB): two saved
  descriptions of the same chain that can disagree on restore.

### 5. 🟡 Legacy alias surfaces (three layers deep)

Same setting, multiple entry points kept for back-compat — each a place for behavior to drift:

- **Engine facade**: `getDeviceManager()` ≡ `getInputDeviceManager()`;
  `setInputDeviceType()` ≡ `setDeviceType()`; `DeviceOptions.type` ≡ `inputType`;
  single-source methods (`setInputGain`, `setMonitorMute`, `setChart`, `scoreChord`, ~40 of
  them) forward to `source0()` while a parallel indexed API (`getSource(id)` →
  `audio:setSource*`) does the same thing for id 0. Two IPC routes mutate the same atomic
  (`audio:setMonitorMute` vs `audio:setSourceMonitorMute(0, …)`).
- **Settings shape**: legacy `{type}` vs `{inputType, outputType}` normalized in **two
  places** — `audio-bridge.ts normalizeDeviceSettings` *and* `screen.js
  normalizeDeviceSettings` (duplicated logic, must stay in sync by hand).
- **Schema/branding**: `feedBack.audio_effects.chain_plan.v1` + accepted legacy
  `slopsmith.…` id; `window.feedBackDesktop` + `window.slopsmithDesktop`; localStorage keys
  still `slopsmith-*`. Each alias doubles the grep surface for every future change.

### 6. 🟡 Duplicated implementation code

- **Three packed-LR SPSC rings** in `AudioEngine.h` (split-mode `outputPendingRing`, renderer
  bus, stream sink) — same pack/unpack, same power-of-two asserts, same drop-oldest logic,
  three hand-maintained copies. One templated ring kills ~2/3 of the index math.
- **Two fail-soft wrappers per method** in the JS layer: audio-bridge's typeof-guarded
  handlers and the executor's `trySetGain`/`trySetMonitorMute`/… re-wrap the same native
  calls with slightly different error policy (bridge: silent no-op; executor: outcome
  strings). A single native-call helper with one policy would remove a class of divergence.
- **`normalizeLoadResult` tolerance duplicated**: both rig_builder (`screen.js`: "Some JUCE
  bridges return {success:false} or bare …") and the executor normalize loadPreset results
  independently.
- **Chain-restore logic**: executor rollback (`rollbackPreset` + `restorePreset`) vs
  screen.js `_restorePresetBlob` — two snapshot/rollback implementations for the same chain.

### 7. 🟠 `startAudio` / route lifecycle from two sides

`audio:startAudio` is invoked by the renderer UI **and** best-effort by the executor when a
chain plan carries `startAudio: true` (`:574`). Neither side knows the other's intent; there's
no matching stop ownership — `releaseRoute` clears the chain and mutes but leaves the device
running or not depending on who started it.

---

### Recommended direction (for TLC scoping, not yet implemented)

1. **Single chain owner**: make the audio-effects executor the *only* writer of the native
   chain; migrate the audio_engine screen's direct loadVST/loadPreset/tone-switch calls onto
   route-scoped executor operations; then delete rig_builder's transient-kill hacks and the
   legacy direct `loadPreset` path. Executor should reject/re-sync when
   `getChainState` disagrees with its `stageSlots` map (generation counter on the native chain).
2. **Arbitrated monitor state**: replace raw `setMonitorMute` writes with a small state owner
   (user preference + N stackable suppressions/overrides, refcounted). `releaseRoute` releases
   its override instead of forcing `true`.
3. **Sanitize all gains natively**: extend `sanitizeStreamGain`-style clamping to
   input/chain/output/backing in `AudioEngine` setters (single choke point) and drop the
   JS-side clamp divergence.
4. **One persistence store per setting**: file-backed settings as the single source; treat
   localStorage as a migration source only, delete after import. Move mute flags out of the
   device blob.
5. **Deprecation plan for aliases**: freeze `slopsmith*` surfaces, log-once on use, remove on
   next major.

---

# Part III — Deep Read: AudioEngine.cpp + NodeAddon.cpp

Full read of `src/audio/AudioEngine.cpp` (3223 lines) and the load-bearing regions of
`src/audio/NodeAddon.cpp` (3699 lines). Line refs current as of
`fix/loopback-capture-permission`.

**Overall verdict first**: the RT core is in much better shape than its size suggests —
disciplined lock-free SPSC rings, no allocation on the audio thread, denormal flushing on every
callback clock, a correct per-deviceKey quiescence handshake for source removal, and unusually
good comments that cite the bug each guard fixes. The problems live at the *edges*: the JS↔native
boundary, concurrency between async workers, and inconsistent input sanitization.

---

### 1. 🔴 Chain-mutating async workers are not serialized (NodeAddon)

`LoadPresetWorker`, `LoadVSTWorker`, `LoadNAMWorker`, `LoadIRWorker`, `ReplaceIRWorker` all queue
on the libuv threadpool (default 4 threads) with **no mutual exclusion between workers**.
`SignalChain` locks per-operation only, so the sequence `clear() → addProcessor() × N`
(`NodeAddon.cpp:3312-3408`) is not atomic.

Two `loadPreset` calls in flight — which is precisely the documented rig_builder-vs-bundle
"~1ms later" race from the collisions doc — can interleave as:

```
worker A: clear()          worker B: clear()
worker A: add(ampA)        worker B: add(ampB)
worker A: add(irA)   →  final chain: [ampA, ampB, irA, irB]  (merged garbage)
```

Both report `success:true` with wrong `slotsLoaded` semantics; the executor's stageId→slotId map
is then built against a chain that neither caller described. A `loadVST` concurrent with a
`loadPreset` similarly lands a slot into (or after) someone else's rebuild.

**Fix shape**: one native "chain mutation" mutex (or a serial dispatch queue) around
clear+rebuild and single-slot adds; alternatively a chain generation counter returned to JS so
callers detect they lost the race. This is the single highest-value fix of the whole pass —
it converts the plugin-vs-plugin fight from corruption to last-writer-wins.

### 2. 🔴 Argument sanitization is inconsistent across the N-API surface

The addon knows the hazard — `getValidatedSource` (`NodeAddon.cpp:89-103`) documents that
`Int32Value()` coerces NaN→0, and `setAudioDevices` normalizes sampleRate against "NaN slipping
past N-API" (`AudioEngine.cpp:700-708`). But that rigor is only applied to the *newer* bindings:

| Guarded (fail-soft) | Unguarded (blind `As<>()` coercion) |
|---|---|
| `getValidatedSource` (all `*Source*` methods) | `SetGain` — NaN/Inf reaches `outputGain.store()` raw |
| `SetSlotState` (IsNumber/IsString checks) | `SetParameter`, `SetBypass`, `RemoveProcessor`, `MoveProcessor` — NaN slotId → **slot 0** |
| `SetMonitorMuteSuppressed`, `SetMonitorKill` (bridge-side Boolean coercion) | `SetMultiBypass` (per-item `As<Number>` uncheck) |
| `setBackingSpeed` (isfinite + clamp, engine-side) | `SendMidiToSlot` (channel/program unclamped → JUCE assertions) |

Consequences of the worst one: a NaN master gain via `audio:setGain('output', NaN)` multiplies
the entire device output to NaN (`buffer.applyGain(outputGain.load())`,
`AudioEngine.cpp:2407/3083`) — full silence plus poisoned peak meters, and nothing scrubs it
(the per-source NaN scrub runs *before* the master gain). Engine-side clamps at the four gain
setters (mirroring `sanitizeStreamGain`) fix every caller at once.

### 3. 🔴 `wasRunning` race in `setAudioDevices` (AudioEngine.cpp:658)

`audioDeviceStopped()` clears `audioRunning` on **transient** stops, and the code's own comment
says WASAPI exclusive opens "routinely fire one mid-start". `setAudioDevices` captures
`wasRunning = audioRunning.load()` and only calls `startAudio()` at the end when it was true.
The comment above it (`:651-657`) fixes the *detach* half of this race (stopAudio is now
unconditional) but the *restart* half still reads the racy flag: a reconfigure landing inside a
transient-stop window sees `wasRunning == false` and leaves the engine configured but stopped —
"no audio until user presses Start/Apply again". The intent flag it should read is "did the user
want audio running", which currently doesn't exist separately from device state (see §6).

### 4. 🟠 `setRendererBus(false)` violates the ring's own SPSC discipline

`AudioEngine.h` (`setRendererBus`) drops buffered audio on disable by writing
`rendererBusReadIndex` from the **control thread**, while `pullRendererBus`
(`AudioEngine.cpp:3143-3208`) is the designated single consumer-side writer of that index (the
file's comments elsewhere are explicit that "only the consumer ever moves readIndex"). A
concurrent output callback mid-`pullRendererBus` can overwrite the control thread's store with
`r + pull`, replaying a stale tail after re-enable — exactly what the drop was meant to prevent.
Low probability, audible-blip severity; fix by setting a "flush requested" atomic the consumer
honors instead of writing its index.

### 5. 🟠 Latency accounting has three unreconciled truths

- `getLatencyMs()` (`:469-496`): device latencies + (split only) a static `kOutputRingFrames/2`
  ≈ 42.7 ms ring-residency guess. The actual ring fill is measurable (`getDeviceMetrics` reports
  it) but not used.
- Verifier auto-offset (`extraInputAboutToStart`, `:2554-2568`): per-device *input-latency
  delta* only, 0 on JACK/PipeWire (documented), user offset summed on top.
- Renderer bus: adds `kRendererBusPrimeFrames` (~10 ms) prime + fill drift + producer-side
  resample, none of it surfaced in any latency figure; stems audio through the bus is delayed by
  an amount the UI never reports and the verifier never compensates.

For a TLC pass: one `getLatencyBreakdown()` that owns all terms would replace three ad-hoc sums.

### 6. 🟠 `audioRunning` conflates user intent with device state

Writers: `startAudio`/`stopAudio` (user intent), `audioDeviceAboutToStart` (device came up —
including JUCE auto-restarts the user never asked for, `:1802`), `audioDeviceStopped` (device
went down — including transient stops the user didn't ask for). Readers assume different
meanings: `setAudioDevices` reads it as intent (§3), detection guards read it as device state
(correct), the bridge's `isAudioRunning` surfaces it to the UI as intent. Two booleans
(`userWantsAudio`, `deviceRunning`) would kill the §3 race and make the auto-restart paths
self-explanatory. Related: `stopAudio()` does not stop the backing transport — `backingPlaying`
stays true and playback resumes on the next start, which is intentional for unplug-recovery but
surprising for an explicit user stop.

### 7. 🟡 Probe/apply duplication — three copies of the rate-tolerance logic

The `|r - r2| <= 0.5` sample-rate matching + round-to-nominal logic exists in
`probeDeviceOptionsDual` (`:316-350`), `applySplitSetup::rateSupportedBy` (`:961-982`), and the
post-open verify (`:1146`). The comments at each site narrate keeping the three in sync by hand
("<= 0.5 (not <) to match…", "Tolerance matches the probe-side rounding…") — i.e. they've
already been bitten. Same story for empty-name→first-enumerated resolution (probe, preflight,
apply must agree; three sites). One shared helper each.

### 8. 🟡 Device identity is display-name only (documented limitation)

`getBindableInputDevices` (`:120-161`) and `bindInputDevice`'s duplicate/primary checks compare
`juce::String` names. Two identical interfaces collapse to one entry; a device exposed under two
backends may bind the wrong one. The comment block is honest about it; flagging here because the
fix ((typeName, name) identity threaded through bind/reopen/persistence) also touches the
renderer's saved settings — a cross-layer change worth scheduling deliberately.

### 9. 🟡 NodeAddon miscellany

- **`LoadPresetWorker` state restore bypasses `setSlotState`**: it `const_cast`s the slot from
  `getSlot()` and calls `slot->setState(state)` directly (`:3402-3404`) — outside whatever
  synchronization `SignalChain::setSlotState` provides against a concurrently-processing audio
  thread. During a preset load the chain was just rebuilt so the window is small, but it's the
  only chain mutation in the file that dodges the class's own API.
- **Every preset load closes every editor window** (`LoadPreset:3452`, `ClearChain:2668` —
  required by the #56 use-after-free). Combined with the tone auto-switch calling loadPreset on
  song events, a user tweaking an amp editor mid-song has the window yanked. A single-slot
  replace path (`replaceProcessor` exists) for tone switches would avoid the nuke.
- **macOS is a second-class citizen by design**: no JUCE dispatch loop (`startJuceMessageThread`
  JUCE_MAC branch), `dispatchOnMessageThread` runs inline, VST/AU instantiation "given up until
  a proper libuv-based pump lands". Every load path carries a divergent `#if JUCE_MAC` branch —
  a large, mostly-untested platform fork woven through the file.
- **`loadVstSandboxAware` holds a libuv worker for the whole plugin init** (documented tradeoff,
  `:2241-2248`); concurrent slow loads can starve fs/crypto AsyncWorkers.
- **Misnomer**: `inputOverflowCount` is incremented by the *output consumer's* catch-up on the
  primary split ring (`:2941`) — it counts ring overruns, not input overflows; the metric name
  leaks into `DeviceMetrics`/diagnostics.

### 10. What is genuinely solid (don't "fix")

- The per-deviceKey `callbacksInFlight` handshake + `pendingRelease` deferral for source removal
  (`:1554-1618`) — correct, well-reasoned, and the 200 ms bounded wait is the right call.
- Ring discipline: packed-LR single-atomic frames, consumer-side drop-oldest, `w < r` resync
  after index resets, consume-vs-pull split to avoid clock skew after clamps (`:2944-2974`).
- RT allocation hygiene: every scratch pre-sized in about-to-start, every hot path clamps to
  capacity instead of resizing; stream scratches deliberately fixed at ring capacity so a
  hotplug about-to-start can't realloc under a live producer (`:1830-1841`).
- `ScopedNoDenormals` on *both* callback clocks with the explanation of why (`:2268-2273,2898`).
- Backing speed hand-off (pending atomic + same-block stretcher reset, `:1670-1691`) and the
  read-ahead thread rationale, including the honest note about `BufferingAudioSource`'s residual
  lock (`:1334-1341`).
- `bindInputDevice`'s failure hygiene: every abort path closes the half-open device; validate-
  eagerly-then-close when the engine is stopped (`:2774-2783`).

---

### Priority order for the TLC pass

1. **Serialize chain mutations** (§1) — prerequisite for any single-chain-owner work from the
   collisions doc; without it the executor can't even trust its own load result.
2. **Sanitize gains + slot ids natively** (§2) — small, mechanical, kills a user-visible
   silence-the-app bug class.
3. **Split `audioRunning` into intent + state** (§6, fixes §3) — unlocks correct
   reconfigure-under-transient-stop and clarifies every auto-restart path.
4. **Renderer-bus flush flag** (§4) — one-line-ish, closes the last SPSC discipline hole.
5. **Latency breakdown API** (§5) and **probe/apply shared helpers** (§7) — quality-of-life,
   schedule with the settings-ownership work.

---

# Part IV — Decomposition / Refactor Plan
Targets the two monoliths:
`AudioEngine.{h,cpp}` (819 + 3223 lines) and `NodeAddon.cpp` (3699 lines).
Builds on the findings in Part II and Part III —
several fixes there (chain-mutation serialization, gain sanitization, intent/state split)
get a natural home in the new units instead of being bolted onto the monolith.

**Precedent**: the SourceChain extraction already proved the working method on this codebase —
move a cohesive member cluster verbatim into a class, bind shared engine atomics by reference,
keep the facade byte-identical, land in phases. This plan repeats that recipe seven more times.

**Prime directive**: no behavior change per phase. Every phase is a pure code move that
compiles + passes the existing tests (`tests/audio_sanitize`, `tests/sandbox/*`, e2e) before
the next starts. Bug fixes ride in separate commits on top of the phase that creates their home.

---

### 1. Target layout

```
src/audio/
  engine/
    AudioEngine.{h,cpp}        facade + callback orchestration only (~500 lines total)
    DeviceSetup.{h,cpp}        probe/apply/teardown for duplex + split      (§2.1)
    SourcePool.{h,cpp}         source add/remove/reclaim + in-flight counts (§2.2)
    ExtraInputs.{h,cpp}        InputDeviceSlot registry, bind/unbind/reopen (§2.3)
    BackingPlayer.{h,cpp}      transport + stretch + leveler + playhead     (§2.4)
    StreamSink.{h,cpp}         2nd output device + submix compose           (§2.5)
    RendererBus.{h,cpp}        WebAudio→engine ring, push/pull/metrics      (§2.6)
    PackedStereoRing.h         the one SPSC ring template                   (§2.7)
    EngineState.h              shared atomics: rates, block sizes, run state(§2.8)
  addon/
    NodeAddon.cpp              module init + binding registration only
    AddonContext.{h,cpp}       engine/vstHost lifetime, message thread, shutdown latch
    NapiHelpers.h              arg validation (the getValidatedSource pattern, generalized)
    ChainOps.{h,cpp}           chain workers (LoadPreset/VST/NAM/IR) + mutation queue
    DeviceBindings.cpp         device enumeration/config/metrics bindings
    ControlBindings.cpp        gain/mute/gate/stream/renderer-bus bindings
    DetectionBindings.cpp      pitch/chord/chart/verdict/source-indexed bindings
    BackingBindings.cpp        backing-track bindings
    EditorWindows.{h,cpp}      PluginEditorWindow + open/close/promotion
  (existing DSP files stay where they are)
```

CMake: append the new files to the existing source list in `src/audio/CMakeLists.txt`; no
target restructuring needed.

---

### 2. AudioEngine decomposition (one phase per unit)

Ordering is by extraction risk, lowest first. Each unit lists what moves, its boundary, and
which known bug lands in it afterwards.

#### 2.1 `PackedStereoRing<Frames>` — first, everything else builds on it

Template over capacity; owns `array<atomic<uint64_t>>`, write/read indices, the pack/unpack
helpers, and the three ritual moves currently copy-pasted at six sites: producer publish
(`packStereoIntoRing`), consumer `w < r` resync, lapped catch-up with overflow counter, and the
pull-vs-consume split. Replaces: `outputPendingRing`, each `InputDeviceSlot::ring`,
`streamSink.ring`, `rendererBusRing`. The static_asserts move inside the template.
**Bug fixed here after the move**: none — but §2.6's flush fix becomes a one-method addition
(`requestFlush()` honored by the consumer) instead of index surgery.

#### 2.2 `SourcePool`

Moves: `sources[]` array, `sourcesMutex`, `callbacksInFlight[]`, `pendingRelease[]`,
`addSource/removeSource/reclaimPendingReleases/listSources/getSource`, the fan-out helpers
(`setMlNoteDetectionEnabled`, `setMonitorKill` loop, `resetPeaks` loop), and
`mixSourcesForDevice`. Boundary: callbacks call `pool.enterCallback(deviceKey)` /
`pool.exitCallback(deviceKey)` (RAII guard) and `pool.mixForDevice(...)`.
This is the most delicate move (RT-shared state) but it is also the best-commented, most
self-contained cluster — the handshake logic doesn't touch any other member.

#### 2.3 `ExtraInputs`

Moves: `InputDeviceSlot` + `extraInputs[]`, `bindInputDevice/unbindInputDevice/
closeExtraInputDevice/reopenDesiredExtraInputs/activeExtraInputCount`, the extra callback
trio (`extraInputCallback/AboutToStart/Stopped`). Depends on SourcePool (prepares/releases
sources by deviceKey) and PackedStereoRing. The (typeName, name) device-identity fix
(deep-read §8) lands here later without touching the engine again.

#### 2.4 `BackingPlayer`

Moves: transport + reader + read-ahead thread, signalsmith stretch state, `backingLock`,
speed hand-off atomics, `BackingLeveler`, `renderBackingBlockLocked`, all playhead caches,
load/start/stop/seek/speed. Boundary: `backing.renderInto(buffer, numSamples)` returning frames
(caller mixes + meters), `backing.prepare(sr, bs)` from the about-to-start hooks. The duplex and
split callbacks already share `renderBackingBlockLocked`, so the seam exists.
**Lands here later**: the "stop engine ≠ stop backing" intent decision (deep-read §6 note).

#### 2.5 `StreamSink`

Already 80% a struct — promote to a class owning its manager, callback, ring, scratches,
`composeAndPushStreamMix`, `set/clear/reopen/close`. Bus flags (`includeBacking/includeGuitar/
gain`) move in. The engine's callbacks call `sink.publish(guitarMix, backing, renderer, n)`.

#### 2.6 `RendererBus`

Moves: ring + indices + resampler carry-state (`rendererBusSrcPos/PrevL/PrevR`), prime/fill
constants, `pushRendererAudio/pullRendererBus/getRendererBusMetrics/setRendererBus`.
**Bug fixed here after the move**: the control-thread readIndex write on disable (deep-read §4)
becomes an atomic `flushRequested` flag consumed in `pull()`.

#### 2.7 `DeviceSetup`

Moves: `probeDeviceOptions[Dual]`, `applyDuplexSetup`, `applySplitSetup`, `teardownSplitMode`,
type resolution/preference tables, and the three hand-synced helpers extracted once:
`rateIntersection()`, `bufferIntersection()`, `resolveDeviceName()` (deep-read §7). Stateless
apart from references to the two managers + EngineState; takes managers by reference so it owns
no lifetime. `setAudioDevices` stays on the facade as the orchestrator (stop → resolve →
duplex-or-split → restart) but shrinks to ~40 lines.

#### 2.8 `EngineState`

Tiny header: `currentSampleRate`, `inputBlockSize`, `outputBlockSize`, `duplexMode`, and —
**the deliberate fix from deep-read §3/§6** — `userWantsAudio` (intent, written only by
start/stopAudio) split from `deviceRunning` (state, written by the device callbacks).
SourceChain already binds engine atomics by reference; it re-binds to this struct unchanged.
Every unit above takes `EngineState&`, which is what keeps them unit-testable without JUCE
devices (hand them a state struct + a fake ring).

#### What remains on `AudioEngine`

The `AudioIODeviceCallback` implementations (now ~60 lines each: enter pool guard → mix →
backing → renderer bus → sink publish → master gain → meters), the facade forwarding to
`source0()` (unchanged for NodeAddon compatibility), device enumeration getters, and
construction/destruction ordering. Header drops from 819 lines to roughly 250.

---

### 3. NodeAddon decomposition

The file is 100+ bindings sharing four bits of infrastructure. Split infrastructure first,
then the bindings become mechanical moves.

#### 3.1 `AddonContext` — lifetime + threading

Moves: `engine/vstHost` globals + mutexes + `snapshotEngine/snapshotVstHost`, the JUCE message
thread (`startJuceMessageThread/stop/dispatchOnMessageThread` with the macOS fork in ONE place),
`alreadyShutDown`, `doShutdown`, `registerPendingLoad/cancelAllPendingLoads`. Everything else
receives `AddonContext&`. This quarantines the `#if JUCE_MAC` platform fork (deep-read §9) into
a single file instead of a branch inside every load path.

#### 3.2 `NapiHelpers.h` — kill the validation inconsistency structurally

Generalize the `getValidatedSource` pattern into typed extractors:

```cpp
std::optional<int>    argSlotId(info, i);     // finite integer, >= 0
std::optional<float>  argGain(info, i);       // finite, clamped 0..8 (one policy)
std::optional<float>  argParamValue(info, i); // finite, clamped 0..1
std::optional<bool>   argBool(info, i);
```

Then rewriting `SetGain/SetParameter/SetBypass/SendMidiToSlot/SetMultiBypass` onto them is the
deep-read §2 fix, done once, enforced by convention (new bindings have no raw `As<>()` path to
copy). Engine-side clamps in the gain setters stay as the second belt.

#### 3.3 `ChainOps` — the serialization point

Moves: all five chain workers + `loadVstSandboxAware` + `decodeStateBlob`. Adds the
**chain-mutation serializer** (deep-read §1): a single `std::mutex chainMutationMutex` acquired
for the full Execute() of every worker, plus a monotonic `chainGeneration` bumped on every
mutation and returned in load results — the executor and renderer can then detect a lost race
instead of trusting a corrupted merge. The `const_cast` slot-state bypass (deep-read §9) is
replaced with `setSlotState()` during this move.

#### 3.4 `EditorWindows`

Moves: `PluginEditorWindow`, the window map, open/close/destroy-on-message-thread, and the
sandbox-promotion flow inside `OpenPluginEditor`. Later improvement lands here: tone-switch
single-slot replace instead of close-all-editors (collisions/deep-read editor-nuke issue).

#### 3.5 Binding files

`DeviceBindings/ControlBindings/DetectionBindings/BackingBindings` — pure moves, grouped to
match the preload API sections, each ~400-600 lines. `NodeAddon.cpp` keeps only `Init/Shutdown`
and the `exports.Set(...)` table (which doubles as the API index the current file lacks).

---

### 4. Phase 0 — Compatibility contract & test scaffolding

Runs BEFORE any code moves. Purpose: turn "no public API change" from a review rule into a
failing CI check, and codify the compat decisions consumers (core screen, rig_builder,
note_detect, stems) depend on. Test infrastructure follows the repo's existing two-track
convention: native `tests/<name>/test.cpp` targets registered in `tests/CMakeLists.txt`, and
Node `tests/*.test.js` for the JS/addon boundary.

**0.a Contract snapshots (`tests/contracts/`)**

| Snapshot | Source | How |
|---|---|---|
| `addon-exports.json` | `slopsmith_audio.node` export table | Node script: `Object.keys(require(addon)).sort()` |
| `preload-audio-api.json` | `window.feedBackDesktop.audio.*` + `audioEffects.*` key lists | static extraction from `preload.ts` |
| `ipc-channels.json` | every `ipcMain.handle`/`ipcMain.on` name in `audio-bridge.ts` | static extraction |
| `result-shapes.json` | golden key/type shapes (not values) for `loadPreset`, `loadVST`, `loadNAM/IR`, `getChainState`, `savePreset`, `getDeviceMetrics`, `getRendererBusMetrics`, and every executor outcome (`loadChainPlan`/`releaseRoute`/`setRouteGain`/…) | run against the real addon (null audio device) + executor with a stubbed native |

CI job `contract-check` regenerates all four and diffs against the committed snapshots.
Additive keys require a deliberate snapshot update in the same PR; removals/renames fail.

**0.b Compat decisions codified as tests**

- `isAudioRunning` reports **device state** (current semantics) — pinned by a test across a
  simulated transient stop, so the phase-1 intent/state split can't silently change it.
- Native gain clamp bounds = **0..32** (matching the executor's `clampGain`), NaN/Inf rejected
  universally; only stream/renderer-bus gains keep the tighter 0..8 `sanitizeStreamGain`.
  Pinned by a table-driven test so phase 8's clamps can't under-shoot a legit rig gain.
- Concurrent `loadPreset` storm test written NOW (expected-fail / quarantined): two overlapping
  loads must end with the chain equal to exactly one caller's preset. Documents today's
  corruption, flips to expected-pass at phase 7, and doubles as the rig_builder timing smoke
  (its transient-kill/unmute heuristics tolerate serialized latency — assert its fallback
  unmute path still fires).

**0.c Native unit-test harness**

Add a `tests/engine_units/` CMake target (same pattern as `tests/audio_sanitize`) that links the
audio sources without a real device — the home for every per-unit test below. Add a tiny
`FakeClock`/`NullDevice` helper pair here once; all later phases reuse them.

### 5. Bespoke tests per phase (unit + integration)

Each extraction phase ships WITH its tests in the same PR — the unit tests pin the moved logic,
the integration gate proves the seam. "U" = `tests/engine_units` C++ test, "I" = Node/e2e.

| Phase | Unit tests (new) | Integration tests |
|---|---|---|
| 1 `PackedStereoRing` | U: SPSC threaded stress (producer/consumer at different block sizes); wrap + drop-oldest lap; `w < r` resync after index reset; L/R tear check under lap (packed-atomic invariant); pull-vs-consume skew accounting; overflow/underflow counters | I: existing audio smoke (duplex, split, stream, renderer bus pass audio); contract-check green |
| 1 `EngineState` | U: intent/state transition table — user start/stop × device aboutToStart/stopped × transient stop; `isAudioRunning` compat pin from 0.b stays green | I: reconfigure-during-transient-stop scenario (documents deep-read §3; expected-fail until phase 8) |
| 2 `RendererBus` | U: resampler continuity across pushes (fractional pos + carried frame → no discontinuity at chunk seams); equal-rate degenerate path bit-exact; prime gate (no output until ~10 ms); underflow → silence + re-prime; fill clamp trims to prime target; flush-on-disable drops tail (expected-fail until phase 8 flag fix); metrics arithmetic | I: `getRendererBusMetrics` shape + push/consume accounting via addon against null device |
| 2 `StreamSink` | U: submix compose matrix (guitar/backing/renderer × include flags × gain); oversized-block skipped AND counted; scratch-not-sized skip is silent-safe | I: OBS-capture manual smoke; stream under/overflow counters via IPC |
| 3 `BackingPlayer` | U (synthetic reader source): speed change adopts rate + stretch reset in same block; EOF short-read playhead clamp; stretch-latency compensation vs 1× bypass; leveler re-prepare on SR change; tryLock-miss drops block without state damage | I: existing backing play/seek/speed e2e, duplex + split; `audio-chain-persistence.test.js` green |
| 4 `DeviceSetup` | U: `rateIntersection`/`bufferIntersection`/`resolveDeviceName` helpers — incl. the 0.5 Hz boundary cases the three duplicated sites hand-narrate today, midpoint-rounding fail-closed cases, empty-name resolution parity | I: manual device matrix (WASAPI shared/exclusive, ASIO, dual-type split); probe verdict == apply outcome assertion in a scripted run |
| 5 `SourcePool` | U: threaded add/remove storm under a fake callback loop — per-deviceKey quiescence handshake, deferred release + reclaim, no release while in-flight (TSAN job on this target); active-snapshot consistency in `mixForDevice` | I: `multi-source.test.js` + `tests/sandbox` e2e (GP-5 scenario), remove-under-load |
| 5 `ExtraInputs` | U: bind rejection matrix (duplicate name, primary device, duplex mode, out-of-range key); transient close keeps intent, permanent unbind clears + deactivates; reopen-failure ghost-source cleanup | I: second-interface e2e; meters zeroed while device gone |
| 6 `NapiHelpers` | I (Node, real addon): table-driven arg fuzz per extractor — NaN/Inf/negative/string/missing/object for slot ids, gains, params, midi bytes → no crash, documented no-op or clamp; pins the 0.b clamp decisions | I: addon init→shutdown→init cycle; pending-load cancellation on shutdown |
| 7 `ChainOps` | U: serializer — N threads × (loadPreset/loadVST/clearChain) → final chain equals exactly one caller's request, `chainGeneration` strictly monotonic, per-caller result reports the generation it produced | I: 0.b storm test flips to expected-pass; `audio-effects-executor.test.js` extended — executor detects stale generation; rig_builder legacy-path telemetry smoke; editor open/close + sandbox promotion e2e |
| 8 bug fixes | U: gain clamp tables (0..32 native, NaN reject); renderer flush flag; `wasRunning` intent read (phase-1 expected-fail flips to pass); latency breakdown terms sum | I: full regression: all snapshots + all suites green |

Cross-cutting:

- **TSAN/ASAN lane** for `tests/engine_units` in CI (the ring/pool tests are exactly what
  sanitizers are for; the RT code has never had one).
- **Expected-fail discipline**: known bugs get their test at the phase that creates the home,
  marked expected-fail with the deep-read § reference; the fix commit flips the mark. No fix
  lands without its test having existed first.
- Existing suites (`audio_sanitize`, `chordscorer`, `mlnotedetector`, `sandbox/*`,
  `*.test.js`) run on every phase — they are the behavior-freeze net.

### 6. Phasing & verification

| Phase | Content | Risk | Gate |
|---|---|---|---|
| **0** | **Contract snapshots + compat pins + `tests/engine_units` harness + storm test (expected-fail)** | **none (test-only)** | **`contract-check` job green; snapshots committed; harness builds on all 3 platforms** |
| 1 | `PackedStereoRing` + `EngineState` (incl. intent/state split behind a facade-compatible `isAudioRunning`) | low | phase-1 unit tests + audio smoke: duplex, split, stream sink, renderer bus each pass audio |
| 2 | `RendererBus`, `StreamSink` | low | phase-2 unit tests; renderer-bus metrics unchanged in diag build; OBS capture works |
| 3 | `BackingPlayer` | low-med | phase-3 unit tests; backing play/seek/speed e2e; split + duplex |
| 4 | `DeviceSetup` | med | helper unit tests; device matrix: WASAPI shared/exclusive, ASIO, dual-type split, probe==apply verdicts |
| 5 | `SourcePool` + `ExtraInputs` | med-high | TSAN-clean pool stress; multi-source + second-interface tests (`tests/sandbox` e2e, GP-5 scenario), remove-under-load |
| 6 | `AddonContext` + `NapiHelpers` | low | arg-fuzz suite; addon init/shutdown cycles, macOS build |
| 7 | `ChainOps` (with serializer) + `EditorWindows` + binding split | med | storm test flips to pass; serializer unit tests; editor open/close, sandbox promotion |
| 8 | Bug-fix commits now homed: gain clamps, renderer flush flag, `wasRunning` intent read, latency breakdown | — | each fix flips its pre-existing expected-fail test |

Every phase additionally requires: `contract-check` green (public surface unchanged) and the
full pre-existing suite green.

Rules that keep this safe:

- **Move, don't edit**: each phase's diff should be reviewable as "same lines, new file" plus a
  thin call seam. The excellent existing comments move with their code.
- **Reference-bind shared state** (the SourceChain trick) rather than adding getters — keeps the
  RT paths free of indirection changes.
- **No public API change**: NodeAddon exports, IPC channel names, and preload surface stay
  identical throughout; the collisions-doc ownership work (single chain owner, monitor-state
  arbiter) is a separate track that starts after phase 7 gives it `chainGeneration`.
- Tester diag counters (`audiodiag`, `[asio-diag]`) must survive verbatim — they're how field
  regressions get caught.

### 7. Explicit non-goals

- Rewriting the JS layer (`audio-bridge.ts` 65K / `preload.ts`) — separate track; its shape
  already mirrors the binding groups this plan creates.
- Replacing JUCE transport/BufferingAudioSource, adaptive resampling for split mode, sandbox
  parameter proxies — feature work, not decomposition.
- Renaming slopsmith→feedBack artifacts — orthogonal, and renaming during a move-refactor
  destroys diff reviewability.

---

# Part V — Merged Priority Roadmap

One ordered list combining the collision remediation (Part II), the deep-read fixes (Part III),
and the decomposition phases (Part IV). Decomposition and fixes interleave: each fix lands as a
separate commit in the unit that becomes its home.

1. **Phase 0** — contract snapshots, compat pins (gain bounds, `isAudioRunning` semantics),
   unit-test harness, concurrency storm test (expected-fail).
2. **Refactor phases 1–2** (`PackedStereoRing`, `EngineState` with intent/state split,
   `RendererBus`, `StreamSink`) — low risk, creates the homes.
3. **Fix: renderer-bus flush flag** (III §4) and **gain/slot-id sanitization** (III §2) —
   small, kills the NaN-master-gain and stale-tail bug classes.
4. **Refactor phases 3–5** (`BackingPlayer`, `DeviceSetup`, `SourcePool` + `ExtraInputs`).
5. **Fix: `wasRunning` intent read in setAudioDevices** (III §3) — now trivial on the split
   intent/state atomics.
6. **Refactor phases 6–7** (`AddonContext`, `NapiHelpers`, `ChainOps` + serializer +
   `chainGeneration`, `EditorWindows`, binding split).
7. **Ownership work (Part II)** — single chain owner via executor (needs `chainGeneration`),
   refcounted monitor-state arbiter, one persistence store per setting, tone-switch
   single-slot replace instead of editor nuke.
8. **Long tail** — latency breakdown API, (typeName, name) device identity,
   slopsmith→feedBack alias deprecation.
