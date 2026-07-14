# Audio Ownership & Mixer Plan — Routes, Leases, and the Engine-Owned Mixer

Follow-up track to `docs/audio-engine-tlc.md` (Part V step 7, "ownership work",
deferred there because it needs the rig_builder repo). Drafted 2026-07-14 on
`refactor/audio-engine-tlc` after the TLC phases + PR #107 review fixes landed.

**Goal**: the audio engine *owns* audio settings, devices, chains, and output.
Everything else — bundled screens, plugins, minigames — is a *client* that
requests routes and holds leases. One ownership model (leases), one output
model (mixer channels), meeting at the route.

Substrate this plan builds on (already shipped on the TLC branch):

| Shipped | Used here as |
|---|---|
| `chainGeneration` + mutation serializer | tamper-evident seal under the lease protocol; becomes per-route |
| refcounted monitor-mute arbiter | precedent + implementation pattern for lease scopes |
| single persistence store (file-backed) | where lease-relevant user prefs (tone engine) live |
| `PackedStereoRing` template | the mixer channel's ring |
| `RendererBus` (ring + resampler + prime gate + fill clamp + metrics) | becomes `MixerChannel`, instantiated N times |
| executor route map + outcome vocabulary + authorization gating | grows into the lease registry |
| rebuild barrier + editor try-lock discipline (PR #107) | unchanged; leases sit above it |

---

## 1. Terminology (binding — settle it before any code)

| Term | Meaning | Today's artifact |
|---|---|---|
| **source** | engine-side pool entry (`SourceChain`, max 8): capture binding + detection + its own `SignalChain` | `sources[i]` |
| **route** | the executor/lease-level name for a source's chain or a mixer channel; what callers request and hold | route key (`desktop-main`) |
| **slot** | a processor position *within* a chain | `ProcessorSlot`, `slotId` |
| **lease** | exclusive, revocable authority over one scope (a route's chain, device config, …) | monitor-mute arbiter (single-scope precursor) |
| **channel** | a mixer input: ring + gain/mute/meter, owned by the engine, held by a client | `RendererBus` (the only one today) |

"Slot" is NOT used for sources/routes anywhere — it already means chain
position and overloading it will corrupt every future review. The user-facing
"player slot" is the `desktop-main` route bound to source 0.

---

## 2. The lease registry

Lives in the **main process** as an extension of `audio-effects-executor.ts`
(it already has routes, outcome strings, and authorization gating). Native
stays dumb: per-route `chainGeneration` remains the detection layer beneath
the polite JS protocol — a lease violation that somehow reaches native is
still caught as a foreign write.

### Two primitives, not one

The conflict inventory (§6) shows two distinct shapes of contention, so the
registry offers two primitives:

1. **Exclusive lease** — conflicting authority; one holder at a time
   (refusal / takeover semantics as below). For: chains, device config,
   transport.
2. **Refcounted demand** — *additive* intent ("I need X on") where multiple
   consumers legitimately overlap; the engine acts while count > 0. For:
   engine-run/capture, detection arming. This is the monitor-mute arbiter
   generalized into a registry primitive — same semantics, one
   implementation instead of one per setting.

### Scopes

Exclusive-lease scopes:

- `device-config` — device/type/sample-rate/buffer AND input-channel select
  (one global scope)
- `signal-chain:<route>` — a specific route's chain, including its noise
  gate (per-route)
- `playback` — backing transport + playhead authority (one global scope;
  the playback screen acquires on song load, releases on exit — verified
  2026-07-14 that splitscreen followers are transport read-only)
- `monitor-state` — already arbitrated; folds into the registry as a scope
- `mixer-channel:<id>` — an output channel's gain/mute (per-channel; the
  holder is whoever requested the channel)

Refcounted-demand scopes:

- `capture` — "the engine must be running and capturing"
- `detection:<route>` — "ML note detection must be armed on this route"

Different holders hold different scopes concurrently. The tuner reading pitch
holds nothing — reads are never lease-gated, only mutations.

### Holder identity is DERIVED, never declared

The main process derives `holderId` from the IPC sender (webContents id +
plugin manifest), never from a caller-supplied string. Main-process /
engine-internal callers use a fixed enum of well-known synthetic ids (§8.4).
Granularity limit: plugins sharing one renderer are distinguished only by
capability-layer attribution — see compound identity, §9. Consequences, all
load-bearing:

- **Death invalidation is automatic**: webContents destroyed → every lease,
  demand, and channel handle it held is released; reload gets a short grace
  window instead (§8.2). No heartbeat protocol needed for the realistic
  failure modes.
- No spoofing, no accidental identity collisions between plugins.
- `getHolder()` and telemetry name the real owner, which is what makes the
  takeover UI and field diagnostics honest.

### Layered values (base + override)

For settings both the user AND a lease holder legitimately write (first
case: the noise gate, §6.2): the user surface writes a persistent **base
preference**; the scope's lease holder may set an **override** that lives
exactly as long as the lease; release (or revocation, or holder death)
restores the base. The monitor-mute arbiter's user-pref-plus-overrides
model applied to arbitrary scalars — implemented once in the registry, used
by any scope that declares a layered setting.

### API shape (executor-level, contract-snapshotted)

```
acquireLease(scope, holderId, opts) -> { granted | refused(holder, reason) }
releaseLease(scope, holderId)
getHolder(scope) -> holderId | null
events: lease-granted, lease-released, lease-revoked, lease-refused
```

### Policy decisions (settled in discussion, 2026-07-14)

- **Refusal by default.** A second caller is refused while the scope is held.
  No silent stealing — that recreates today's races with extra steps.
- **User-initiated takeover** as the only revocation path for contended
  scopes (practically: `desktop-main`). The UI offers "take over"; the
  registry revokes with a `lease-revoked` event so the old holder can
  degrade gracefully. Per-route leases make contention rare — a refused
  caller can usually request its OWN route instead of fighting.
- **Death-triggered invalidation** — the hard requirement. Leases are tied
  to observable lifecycles: webContents destroyed (reload = grace window,
  §8.2), plugin teardown, or executor route release. A wedged/crashed holder
  must never brick a scope until restart. Pattern precedent: vst-crash-guard
  sentinels.

---

### Ownership at a glance

Every aspect of the engine, its owning authority, and who holds what. "Engine"
in the authority column means: the engine is the only mutator; everyone else
goes through the named scope/API. Reads (meters, playhead, chain state,
detection results) are always free.

| Aspect | Authority | Scope / primitive | Typical holder | Everyone else |
|---|---|---|---|---|
| Devices, types, sample rate, buffer sizes | Engine | `device-config` lease | audio_engine device screen | read-only; route requests |
| Input channel select (per source) | Engine | `device-config` lease (per-route later) | device screen | request via route API (6.6) |
| Engine run / capture state | Engine | `capture` demand (refcount) | any consumer needing live input (tuner, minigames, notedetect, bongocat) | raw start/stop = device screen only |
| Signal chain of a route (slots, params, presets) | Engine | `signal-chain:<route>` lease | tone engine (Rig Builder or native) for `desktop-main`; requester for own routes | refused (`held`); takeover UI on `desktop-main` |
| Noise gate (per route) | Engine | layered value on `signal-chain:<route>` | base: user settings UI; override: chain lease holder | refused |
| Monitor mute / kill | Engine | `monitor-state` (arbiter: user pref + overrides) | user pref + transient suppressors | via arbiter only |
| Master / backing / input gains | Engine | user-scoped, native-clamped (TLC fix) | user UIs | last-writer-wins, sanitized |
| Backing transport + playhead | Engine | `playback` lease (global) | playback screen | refused; verifier feed rides the scope's contract (6.5) |
| Detection arming (ML/ONNX) | Engine | `detection:<route>` demand (refcount) | notedetect, minigames, strum-fighter — concurrently | reads (`getActiveDetection`) always free |
| Verifier offsets (per route) | Engine | route's detection scope | calibration UI of the route holder | plugin-local verifiers unaffected (6.7) |
| Mixer: channel lifecycle + audio content | Engine | tier-3 produce handle (§5.1) | the channel's requester | no handle, no writes |
| Mixer: channel gain / mute (the fader) | Engine | tier-2 mix control — NOT leased | the user, via any mixer UI (`audio-mix` capability for plugins) | last-writer-wins, native-clamped, event-synced |
| Mixer: default channel #0 content | Engine | channel #0 produce handle | juce-audio feeder (feedBack repo) | push refused without handle |
| Mixer: which channels StreamSink taps | Engine | structure (tier 4) | stream-settings UI via lease | read-only |
| Settings persistence | Main process | file store (single writer since TLC) | audio-bridge | localStorage = migration source only |

Rule of thumb encoded above: **content and structure are held; faders and
reads are free.**

## 3. Route requests — input AND output through one API

Callers never touch source indices (the `stageSlots` fragility lesson: no
client-side index coupling). They request a route with properties and get an
opaque handle they now hold.

```
requestRoute({ kind: 'input-physical', device, channel })  -> route | refusal
requestRoute({ kind: 'input-virtual', midi: true })        -> route | refusal
requestRoute({ kind: 'output', label, latencyHint })       -> route | refusal
releaseRoute(route)
```

- **input-physical** — a device+channel binding (existing bind rules: pool cap
  8 sources, 3 extra devices, duplicate/primary checks). Comes with a leasable
  signal chain. **Consent-gated**: binding a microphone/interface is a
  privacy-adjacent user-visible act → flows through the executor's existing
  authorization gating (`user-action` / `restore-selection`); a plugin cannot
  silently start capturing.
- **input-virtual** — NEW engine capability (`addVirtualSource()`): a pool
  source with no capture binding, silent input feed, full chain + detection +
  mixer participation. MIDI reaches its chain via the existing per-slot
  `queueMidiMessage`. Unlocks: MIDI-driven minigames (VSTi in the chain),
  metronome, device-setup test tone.
- **output** — a mixer channel: ring + gain/mute/meter, no chain. Cheap;
  granted freely up to a hard cap (~16–32, refusal `no-capacity`); channels
  silent + unfilled for N min are reaped (`channel-removed`), holder
  re-requests transparently on next push.
- **Composite for free**: an input route's chain output IS a mixer channel;
  requesting an input route implicitly yields its output channel.

Refusal vocabulary (extends the executor's outcome strings): `no-capacity`,
`device-unavailable`, `already-bound`, `user-action-required`, `held`.

Asymmetry to encode deliberately: output = cheap + consent-free; input =
scarce + consent-gated.

The device-setup flow creates the physical routes the *user* wires up
(`desktop-main` = source 0); plugins request additional routes.

---

## 4. Tone-engine consolidation

A "tone engine" is simply **the holder of `signal-chain:desktop-main`**.

- New user setting (file-backed store): which provider auto-acquires that
  lease on session start / song load — Rig Builder, Audio Engine native,
  (future providers register through a capability).
- Everyone else's chain writes are *refused*, not raced. Kills, permanently:
  rig_builder's transient-kill/`_rbUnmuteTimer` timing hacks, the audio_engine
  screen's ~30 direct `clearChain` sites, the legacy direct `loadPreset` path
  (`audio-effects.legacy-native-load`).
- **Tone auto-switching moves INTO the tone engine** (decision): switching is
  a tone-engine responsibility, not a separate service. The audio_engine
  bundle's `applyToneMappingsNow` / `applyToneAutomationFor` migrate behind
  the provider interface.
- Cross-repo sequencing: executor lease API lands first (this repo),
  rig_builder migrates second (own repo), legacy path goes log-once-
  deprecated, then dies.

---

## 5. The engine-owned mixer

Every audible thing becomes a channel on one native mixer:

```
Mixer (engine-owned)
  ├─ guitar buses        (input routes' chain outputs — incl. virtual sources)
  ├─ backing player      (engine-internal channel)
  ├─ default channel #0  (permanent: renderer master via loopback capture —
  │                       every sound that doesn't claim a bespoke channel)
  ├─ plugin channels     (stems, metronome, minigame SFX — requested routes)
  └─ → device output; StreamSink taps configurable channel subsets
```

- **`MixerChannel` = RendererBus generalized.** The ring, producer-side
  linear resampler, prime gate, fill clamp, flush flag, and metrics move
  as-is behind a channel registry; per-channel gain/mute/meter on top.
- **Channel #0 is the permanent default, not a compat shim** (decision,
  2026-07-14): the `getDisplayMedia` loopback capture keeps feeding it, so
  any renderer audio that never requests a bespoke channel — legacy plugins,
  UI sounds, one-off `<audio>` tags — still reaches exclusive-mode outputs
  with zero integration work. Bespoke channels are the opt-in upgrade for
  producers that want their own gain/mute/meter/diagnostics.
- **Double-audio guard**: a producer that migrates to a bespoke channel must
  route its WebAudio graph away from the renderer master (its own
  destination), otherwise it plays twice — once through its channel, once
  through #0's capture. The channel-request API docs make this the
  requester's contract; the stems migration is the reference example. A diag
  heuristic (bespoke channel active + #0 meter elevated) flags violations
  (§8.6).
- **Direct tester payoff**: the stems plugin pushes PCM into its own channel
  instead of riding the aggregate #0 path, so a renderer main-thread stall
  no longer starves stem audio behind everything else, and per-channel
  underflow counters mean field logs name WHICH channel starved (the ASIO
  stem-glitch investigation wanted exactly this).
- **Latency**: each channel reports its residency into `getLatencyBreakdown`
  (one owner for every term — the TLC invariant holds).
- **Transport v1 = per-channel IPC push** (today's known-good renderer-bus
  path). Channel handles are opaque so a SharedArrayBuffer ring can replace
  the transport later without API change.
- **Format contract v1**: interleaved stereo float32 at a declared source
  rate, producer-side resample. Multichannel/int16 are non-goals.
- **Reclaim behavior**: channel death (holder gone) fades to silence — never
  a click, never a stuck channel.

### 5.1 Exposed surface — authority stays in the engine, control is tiered

Principle: the engine owns the mixer; callers get *tiers* of access. The key
separation is **producer vs. mix**: the holder controls a channel's content
and lifecycle, but the *fader belongs to the user* — mix controls must work
from any UI without holding anything (OS-volume-mixer model).

| Tier | Who | Gate | Surface |
|---|---|---|---|
| **1. Observe** | anyone | none (read-only) | `mixer.listChannels()` → id, label, kind (`default` / `plugin` / `engine`), holder, gain/mute, meters, fill + underflow counters; events `channel-added` / `channel-removed` / `channel-changed`. Fully event-driven mixer UIs — playback screen, plugins, diag overlay all render from the same feed. |
| **2. Mix** | user-facing UIs; plugins via the existing `audio-mix` capability | capability, NOT a lease | `mixer.setChannelGain(id, v)` / `mixer.setChannelMute(id, bool)`. Engine clamps natively (`sanitizeStreamGain` pattern — no JS-side trust), broadcasts `channel-changed` so every open mixer view stays in sync. Last-writer-wins is CORRECT here (two sliders on one fader is solved UX; unlike chain writers there is no compound state to corrupt). Gains persist per `holderId + label` in the file store (§8.8). |
| **3. Produce** | the channel's holder only | opaque channel handle from `requestRoute` | push audio, flush, latency hint, release. The handle is the authority token; no handle, no writes. |
| **4. Structure** | lease holders | lease scopes (§2) | StreamSink channel taps, device config / sample rate, channel policy. |

Why tier 2 is not lease-gated: routing "turn down the stems" through the
stems plugin's lease would force every producer to reimplement a volume API
and would break the playback screen's mixer whenever a holder is busy or
gone. The engine's native clamp + single event stream keeps authority intact
while multiple UIs share the fader.

Wiring: tiers 1–2 land on the preload surface as `audio.mixer.*` alongside
the existing ~99 methods; IPC channels + result shapes join the phase-A
contract snapshots. The playback screen's in-song mixer and the stem_mixer
plugin both become tier-1/2 clients of the same endpoints — no bespoke
side-channel per consumer.

---

## 6. Ownership conflict inventory — verified against code (2026-07-14)

Sweep of every audio write-API caller across feedBack-desktop, the feedBack
server repo (static/ + plugins/), and the plugin repos. Beyond the chain /
monitor-mute / gains / device-settings conflicts the TLC doc already
catalogued (Part II), these need an ownership declaration:

### 6.1 🔴 `startAudio` — five-plus independent writers, no stop ownership

Callers found: audio_engine screen (multiple), audio-effects-executor
(`startAudio: true` plans), **bongocat** (`run-controller.js` — starts the
engine if not running, with a compensating `stopAudio` if superseded),
**tuner** (`feedBack/plugins/tuner/utils/audio.js` — same start-then-undo
hack), **minigames** (`feedBack/plugins/minigames/screen.js`),
**notedetect** (`screen.js`, 2 sites). Every caller reimplements
"isRunning? → start; remember whether *I* started it" with private undo
logic — five copies of an implicit refcount, each of which can strand the
engine running or stop it under another user.

**Declaration**: raw start/stop becomes user-only (device screen). Everyone
else calls `requestCapture(holderId)` / `releaseCapture(holderId)` — a
refcounted run demand on the lease registry (the monitor-mute arbiter
pattern applied to engine-run intent; the phase-1 `userWantsAudio` split
gave it a natural native anchor). Plan home: **phase A** (registry scope),
migration in **B/C**.

### 6.2 🔴 Noise gate — settings UI vs. nam_tone presets

audio_engine screen: 17 `setNoiseGate` sites (user gate settings UI).
nam_tone `screen.js`: writes gate threshold/enable on preset apply (2
sites). Last-writer-wins; the settings UI silently shows stale state after
any NAM preset applies its own gate.

**Declaration**: the gate is part of the tone — it belongs to the
`signal-chain:<route>` lease holder. The user's gate UI writes a *base
preference*; the tone engine may override per-preset while it holds the
lease, and the base restores on release (exactly the arbiter's
user-pref-plus-overrides model). Plan home: **phase C/D**.

### 6.3 🔴 ML detection arming — boolean toggled by multiple armers

`setNoteDetectionEnabled` / chart+scoring consumers: notedetect (60 call
sites — the primary owner), **strum-fighter** (`game.js`, chord modules),
**highway_3d** (`feedBack/plugins/highway_3d/screen.js`). Arming is a plain
boolean: whichever minigame/screen disarms last kills detection for a
concurrent consumer (and arming is what gates the ONNX inference cost, so a
leaked arm quietly burns CPU forever).

**Declaration**: detection demand becomes a refcounted registry scope
(`detection:<route>`), same pattern as 6.1. `getActiveDetection()` stays
free (read). Plan home: **phase A** scope, consumers migrate in **C/E**.

### 6.4 🟠 Renderer bus / default channel #0 — feeder lives in another repo

`feedBack/static/js/juce-audio.js` owns the loopback/stems/element capture
modes and flips `setRendererBus(enable, gain)` at 8 sites; it is today the
*only* legitimate feeder — but nothing enforces that, and the API is on the
public preload surface next to the ones bongocat already helps itself to.

**Declaration**: channel #0's producer handle (§5.1 tier 3) is held by the
juce-audio feeder module; `setRendererBus`/`pushRendererAudio` become the
handle-scoped produce API and leave the free-for-all surface. Cross-repo:
the feeder lives in the feedBack server repo. Plan home: **phase B**.

### 6.5 🟠 Backing transport + playhead — split-brain across repos

`juce-audio.js` (feedBack repo) drives `loadBackingTrack`/`startBacking`/
`stopBacking`; `transport.js` + `player-controls.js` drive start/stop/seek;
the sloppak path freezes the engine playhead and **notedetect** pushes the
corrected playhead via `setPlayhead` for the verifier. Three modules in two
repos plus one plugin all steer "what time is it" — the TLC doc's
frozen-playhead leak (Part I §5), now with its writer set mapped.

**Declaration**: one transport owner — the playback screen — holding the
global `playback` scope; `setPlayhead` stops being a
free-standing write and becomes part of the detection contract tied to that
scope. Plan home: **phase C**, aligned with the tone-switching migration.

### 6.6 🟠 Input channel selection — notedetect writes device config

notedetect `screen.js` calls `setInputChannel`/`setSourceInputChannel` (4
sites) and reads `loadDeviceSettings` — a plugin mutating what the device
screen owns. Its motive is legitimate (calibration needs the right channel)
but the write path is a silent conflict with the `device-config` owner.

**Declaration**: channel select is `device-config` (or per-route input
config) — notedetect *requests* it through the route API and the grant is
user-visible. Plan home: **phase C**.

### 6.7 🟡 Verifier offsets — parallel calibration surfaces

Core screen sets native per-source verifier offsets; **splitscreen** runs
its own contained-verifier offset UI (plugin-local, benign today); nothing
stops two calibration UIs fighting over the native offset later.

**Declaration**: per-route setting under the route's detection scope.
Low urgency; document now, enforce when 6.3 lands.

### 6.8 Resolution mechanics — three families, one migration strategy

The seven conflicts collapse into the registry's primitives; nothing needs a
bespoke mechanism:

| Family | Mechanism (§2) | Solves |
|---|---|---|
| Refcounted demand | `capture`, `detection:<route>` counters | 6.1 startAudio, 6.3 detection arming |
| Exclusive lease (+ layered values for the gate) | `signal-chain:<route>`, `playback`, `device-config` | 6.2 gate, 6.5 transport/playhead, 6.6 channel select, 6.7 verifier offsets |
| Producer handle | channel #0 tier-3 handle (§5.1) | 6.4 renderer-bus feeder |

**Migration without a flag day — deprecation shims.** Existing surfaces keep
working while callers migrate lazily:

- `startAudio()` / `stopAudio()` from a plugin context become shims over
  `requestCapture` / `releaseCapture` with the derived holder (log-once
  deprecated). The five hand-rolled "did *I* start it?" undo hacks
  (bongocat, tuner, minigames, notedetect ×2) are deleted in their repos as
  they migrate; until then the shim makes them merely redundant, not
  harmful. Raw start/stop stays available to the device screen only.
- `setNoteDetectionEnabled(true/false)` becomes demand acquire/release on
  the derived holder. A holder that leaks its arm loses it on death —
  closing the "leaked arm burns ONNX inference forever" failure by
  construction.
- `setNoiseGate` from the settings UI writes the base preference; from the
  chain-lease holder it writes the override; from anyone else it is refused
  (`held`). nam_tone's preset gate rides its tone-engine lease (phase D).
- `setPlayhead` / backing transport calls check the `playback` scope;
  notedetect's verifier feed becomes part of that scope's detection
  contract rather than a free write.
- `setRendererBus` / `pushRendererAudio` become the produce API on channel
  #0's handle; the juce-audio feeder (feedBack repo) is granted it at
  startup. Enforcement is telemetry-gated (§8.5): unhandled callers log-once
  through phase B, refusal flips only at zero legacy calls.

Every shim logs once per session per caller with the derived holder id —
that telemetry IS the migration progress dashboard, the same trick as
rig_builder's `legacy-native-load` counter.

### Inventory summary

| Conflict | Writers today | Scope it maps to | Phase |
|---|---|---|---|
| startAudio/stop | 5+ (2 repos + 3 plugins) | `capture` refcount | A (+B/C migration) |
| Noise gate | 2 (core UI, nam_tone) | `signal-chain:<route>` | C/D |
| Detection arming | 3+ (notedetect, strum-fighter, highway_3d) | `detection:<route>` refcount | A (+C/E) |
| Renderer bus feeder | 1 legit (feedBack repo), unenforced | channel #0 produce handle | B |
| Backing transport/playhead | 3 modules, 2 repos + notedetect | `playback` | C |
| Input channel select | 2 (device screen, notedetect) | `device-config` / route | C |
| Verifier offsets | 2 (core, splitscreen-local) | route detection scope | C+ |

## 7. Phasing

Same discipline as the TLC branch: contract snapshots first, no behavior
change per move-phase, fixes as separate commits, every phase ships with its
tests, existing suites stay green throughout.

| Phase | Content | Repo(s) | Gate |
|---|---|---|---|
| **A** | Lease registry in executor: scopes, acquire/release/refuse, death invalidation, `getHolder`, events. Contract-snapshot the surface. No behavior change for non-participants. | desktop | holder-death matrix as e2e against real webContents lifecycles (§8.14); contract-check green |
| **B** | **Mixer**: `MixerChannel` from RendererBus (renderer bus = permanent default channel #0, byte-compatible), channel registry (native + IPC), tiered `audio.mixer.*` surface (§5.1, tiers 1–3), output-route requests (cap + idle reap, §8.9), channel groups (§8.13), double-audio diag heuristic (§8.6), stems → own channel (WebAudio graph rerouted off the renderer master — double-audio guard), StreamSink taps mixer | desktop | renderer-bus unit suite passes against channel #0 unchanged; stems-on-ASIO manual test (the tester's 5-stem file) incl. no-double-audio check; mixer surface contract-snapshotted; two concurrent mixer UIs stay in sync via `channel-changed`; per-channel metrics in diag |
| **C** | Chain ownership: per-route `chainGeneration`, audio_engine screen → lease-scoped executor ops, rig_builder migrates, legacy direct path log-once → deleted, takeover UI for `desktop-main` | desktop + rig_builder | storm test asserts *refused not raced*; rig_builder timing hacks deleted; `legacy-native-load` telemetry at zero |
| **D** | Tone-engine selection: user setting, provider registration capability, auto-switching migrates into providers | desktop + rig_builder | switch-engine e2e: mid-session provider swap without audio dropout |
| **E** | Virtual sources: `addVirtualSource()`, input-virtual route requests, MIDI→chain path e2e (VSTi minigame scenario) | desktop | virtual source in pool/mixer/verifier without device; pool-cap refusal test |
| **F** | Cleanup: `slopsmith*` aliases, localStorage keys, frozen legacy surfaces (TLC Part II §5 deprecation plan) | desktop | grep-zero on deprecated surfaces; contract snapshots updated deliberately |

**B before C deliberately**: the mixer is independent of chain ownership,
single-repo, and carries the direct tester-visible payoff (ASIO stems). C/D
are the cross-repo track and can proceed in parallel after A.

### Open items / risks

- Lease heartbeat vs. lifecycle-only invalidation: start lifecycle-only
  (webContents + plugin teardown cover the real cases); add heartbeat only if
  a holder class appears that neither covers.
- `getDisplayMedia` loopback: **kept permanently** as the feed for default
  channel #0 (decision, 2026-07-14) — the zero-integration path every
  renderer sound gets for free. Consequence accepted: the aggregate-path
  failure mode (a renderer stall starves EVERYTHING on #0 at once) remains
  possible for whatever still rides #0; the mitigation is migrating
  stall-sensitive producers (stems first) to bespoke channels, not deleting
  the default. Double-audio during migration is guarded by the requester
  contract in §5.
- Sample-rate authority: the mixer runs at device rate; channels resample
  producer-side (unchanged from RendererBus). `device-config` lease holder is
  the only writer of that rate.
- macOS is **first-class** for all phases (per 2026-07-14 decision); the
  message-thread constraints from PR #107 (try_lock on the message thread,
  serializer coverage for the inline macOS paths) apply to all new native
  surfaces.

---

## 8. Review decisions (2026-07-14, plan review)

Fourteen gaps found in review, each settled. These are binding alongside §2's
policy decisions.

### Lifecycle edges (the blockers — all Phase A)

1. **Revocation handover = drain-then-grant.** Revoke → serializer stops
   accepting the old holder's ops (`lease-revoked` fires here so it can stop
   enqueueing voluntarily) → queued ops drain → route generation bumps → new
   holder granted. No cancel path, no generation-fence-only race.
2. **Reload = grace window.** webContents *reload* (not destroy) suspends the
   holder's leases ~5–10 s keyed on manifest identity; the same identity
   re-requesting restores them; timeout or destroy releases for real and
   notifies waiters. Destroy stays immediate. F5 never changes ownership.
3. **User stop suspends demands.** Raw stop (device screen) always wins:
   engine stops, `capture` demands enter *suspended* (not cleared), holders
   get `capture-suspended` / `capture-resumed` events. Only user start
   resumes. Same ride-through for `device-config` changes that restart the
   engine.
4. **Well-known internal holders.** Main-process / engine-internal callers
   use a fixed enum of synthetic ids (`engine:backing-player`,
   `main:startup-restore`, `main:executor`), registered at boot, lifecycle =
   process (or explicit module teardown). Same registry rules — internal
   callers can be refused, and telemetry names them honestly. No open
   registration API.

### Mixer + migration

5. **Channel #0 migration is telemetry-gated.** `setRendererBus` /
   `pushRendererAudio` stay ungated but log-once with caller id through
   phase B; refusal flips only after telemetry shows zero legacy callers for
   a full release cycle. Never a silence window from cross-repo skew.
6. **Double-audio gets a diag heuristic.** Bespoke channel active AND #0
   meter concurrently elevated → diag-overlay warning + log marker naming
   the channel. No correlation DSP. Phase B gate item.
7. **Demand leaks get diag visibility, not auto-release.** Registry tracks
   per-holder demand age; diag overlay + field logs surface it ("notedetect
   holds detection:desktop-main for 47 min"). Policy may tighten later with
   data; no visibility-tied or timeout-based auto-release now.
8. **Channel gain persistence keys on `holderId + label`**, never label
   alone. Engine-internal channels use the well-known ids from (4).
9. **Channel cap + idle reap** — details folded into §3's output-route
   bullet; referenced from Phase B.

### Scopes + UX

10. **Base pref editable during override.** Layered values accept base
    writes anytime (persist, apply on release); the owning UI shows an
    override-active indicator ("controlled by Rig Builder preset") so the
    audible no-op isn't mysterious.
11. **`playback` is one global scope** — folded into §2/§6.5. Evidence:
    splitscreen followers are transport read-only (one-way BroadcastChannel
    `time`/`playstate` from the main window; popup `<audio>` force-paused).
12. **Startup restore re-binds user-created routes only.** Plugin-requested
    physical input routes are not restored; the plugin re-requests on load
    and the consent gate applies fresh.
13. **Channel groups in Phase B.** Grouped channels share a prime gate /
    common clock reference so producers that need cross-channel sample
    alignment (multi-channel stems, metronome-against-backing) can opt in;
    ungrouped channels stay independent streams with no alignment guarantee.
14. **Death-invalidation tests are e2e.** The Phase A holder-death matrix
    must exercise real webContents lifecycles (crash, destroy, reload,
    navigation) — not a mocked event emitter.

---

## 9. Capability-pipeline integration (2026-07-14)

The renderer capability registry (`static/capabilities.js` + domain hosts,
feedBack repo) is the plugin-facing half of this plan. Verified against code:
the existing domains map near-1:1 onto the plan's scopes, so the plan reuses
them instead of growing a parallel surface.

### Division of authority

- **Capability layer = policy, eligibility, attribution, UX.**
  Manifest-declared roles, safety classes, consent surfaces, compatibility-
  shim telemetry, Capability Inspector.
- **Lease registry = enforcement.** Main process only. The capability layer
  is in-renderer and self-declared (`registerParticipant` is honor-system);
  anything checked only there is bypassable by direct preload callers — the
  exact 6.1/6.4 failure. Enforcement never lives renderer-side only.
- **No mirroring.** The capability layer *reads* lease state (`getHolder`,
  lease events) and renders it; it never caches its own copy of
  who-holds-what. One source of truth, no drift.

### Plugins never see `acquireLease`

Capability commands are the plugin-facing API; domain hosts call the
executor's lease/demand ops underneath. Refusals surface as capability
outcomes. Mapping:

| Plan scope / primitive | Capability domain (exists today) |
|---|---|
| `signal-chain:<route>` lease | `audio-effects` — routes, providers, executors, `select-chain` / `load-plan` / `release-route`, `user-action-required` outcome |
| `playback` scope | `playback` — already tracks `requesterId` + command sequence |
| `detection:<route>` demand | `note-detection` |
| `capture` demand | `audio-input` / `audio-session` |
| Mixer tier 1 | `observer` role (any domain) |
| Mixer tier 2 | `audio-mix` — exists, incl. fader-registry bridge |
| Mixer tier 3 | produce handle issued through the domain host (`stems` domain for the stems producer) |
| Mixer tier 4 | lease scopes (§2) |
| §4 provider selection | `audio-effects` `register-provider` / mappings — largely built; Phase D shrinks to "wire active-provider selection to lease auto-acquisition" |
| §6.8 shim telemetry | `registerCompatibilityShim` — `legacy-native-load` is already one of five registered shims; the Inspector renders migration progress for free |

### Compound holder identity (decision)

§2's derived identity has a granularity limit: all plugins share the main
renderer webContents, so the main process cannot distinguish nam_tone from
notedetect at the IPC boundary. Resolution — **compound identity**:

- **Hard part (enforced)**: webContents id. Enforcement boundaries, death
  invalidation, and the "no spoofing" guarantee hold at this granularity.
- **Soft part (attributed)**: capability-layer plugin id, carried for
  telemetry, diagnostics, and takeover-dialog naming. Plugin-level
  arbitration *within* one renderer is delegated to the capability domain
  host there — as the single dispatcher it can arbitrate honestly among its
  own participants, but this is a trust step, not enforcement, and is
  documented as such.
- Plugin teardown inside a living renderer is only visible to the capability
  layer → plugin-level release is capability-initiated (advisory); the hard
  backstop remains webContents death.

### Alignment items

1. **One outcome vocabulary.** Plan refusals (`held`, `no-capacity`, …)
   extend the capability outcome set (`denied`, `no-owner`,
   `user-action-required`, `stale`, …) — never two parallel vocabularies.
2. **Timeout class for takeover.** Capability command dispatch has
   per-command timeouts; drain-then-grant (§8.1) can exceed them under a
   deep serializer queue. The takeover command gets a longer timeout class
   or completes asynchronously via a lease event.
3. **Cross-repo note.** Domain hosts live in the feedBack server repo; the
   lease registry lands in desktop (Phase A). Domain-host wiring to lease
   ops rides the same telemetry-gated migration as §8.5 — no flag day.
