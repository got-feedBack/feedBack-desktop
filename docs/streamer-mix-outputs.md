# Streamer Mix Outputs — Design & PR Plan

**Status:** charrette complete (6-lane panel: audio-engine, compatibility, devops/operability,
sound-design, growth/marketing, streaming-UX) + a Discord-direct fast-follow round.
**Scope:** a built-in, advanced "streamer mix outputs" feature for the FeedBack **desktop** app —
multiple independent audio mixes routed to OBS *and/or* Discord, **without external routing tools**.
**Date:** 2026-06-28

This is desktop-shell-only (native engine + bridge + renderer Audio page). It does not touch the
web renderer or upstream core. All audio logic stays in C++.

---

## TL;DR

1. **This is far less work than it looks.** Every panelist who read the engine independently reached
   the same conclusion: the native JUCE engine already solved the genuinely hard real-time pieces for
   the *input* side. The feature is largely **"invert Phase-2 to the output side"** — a bus/submix
   layer + extra output sinks. The three requested mixes map onto sources that already exist.
2. **The hard part is not mixing — it's getting a mix OUT to OBS/Discord without shipping a driver.**
   That's the whole design tension, and it resolves to: route a bus to an output endpoint the capture
   tool can already grab, and use **monitor-kill (PR #47)** so the FeedBack process emits *only* the
   stream mix.
3. **OBS and Discord are one mechanism with two presets**, not two features. OBS can carry multiple
   mixes (separate endpoints/tracks); **Discord is inherently one combined mix** per call.
4. **Ship one stream bus first** (serves both the Discord "play for friends" persona and the OBS
   single-feed persona); the multi-mix A/B matrix is OBS-shaped and comes second.

---

## The three target use cases (the brief)

Streamer with an Axe FX III on ASIO — **ASIO in5 = dry DI**, **ASIO in1 = the wet tone** monitored
via their Axe FX *hardware* (≈0 latency to their ears):

1. **LOCAL (me):** hear the **game audio only** (they hear their guitar through their own rig).
2. **STREAM A → viewers:** **game + the DI (in5) re-amped through an in-app NAM/tone** (song-bundled
   or independently chosen).
3. **STREAM B → viewers:** **game + the wet hardware tone (in1)**, no NAM.

---

## What the engine already has (verified in `feedback-desktop/src/audio`)

- The native engine **already mixes game + guitar in one domain**: it loads/plays the song backing
  track (`AudioEngine`: `loadBackingTrack`/`startBacking`/`backingVolume`/`getBackingLevel`) **and**
  processes guitar through a per-input **VST/NAM/IR** chain in `SourceChain`, summing → one output.
- **Dual `AudioDeviceManager`s** (duplex *and* split mode) on **independent clocks**.
- **Lock-free, drift-absorbing SPSC rings** (packed-LR uint64, drop-oldest) — already used to sum up
  to **3 extra input devices**, each on its own hardware clock (`extraInputs[]` / `InputDeviceSlot`).
- **`kMaxSources` (8) pooled `SourceChain`s**, each with `selectedInputChannel` (pick ASIO ch 1 vs 5),
  `deviceKey` (multi-device), and its own tone chain. → "DI→NAM" and "wet, no-NAM" are **each already
  expressible as a source**.
- Safe live teardown handshake (`callbacksInFlight[]`/`pendingRelease[]`), fixed pools, no RT alloc,
  `ScopedNoDenormals`.

**The gap (exactly):** one output device; output channels hard-capped at 2; **no bus/submix concept**;
**no master limiter** (the only limiter is inside `BackingLeveler`, on the backing pre-fader).

---

## Core architecture (consensus: audio-engine + devops + UX)

**Mixer = a fixed pool of N Buses. Each Bus = (tap-set + per-tap gain) → one Sink.**

- **Tap-set** = a bitmask over **already-existing** `SourceChain`s + a **backing/game** flag. No new
  per-bus DSP — "DI→NAM vs wet" is just *which source* a bus includes.
- **Sink** = `OutputDeviceSlot`, a near-exact mirror of the existing `InputDeviceSlot` (own device
  manager + output callback + packed SPSC ring + desired-vs-active intent).
- **Render once, sum N times.** Each source already renders once; buses select subsets. Cost is a
  handful of MACs/block (~<1% CPU) — bus count does **not** multiply the NAM/IR/backing cost.
- **The one crux decision (must settle in PR1):** backing currently renders on the *listener's* clock
  under `backingLock` and **advances the transport** there. N sinks each rendering backing would
  **double-advance the playhead** (a correctness bug). → **Render backing once on the master clock and
  fan it via ring to the stream sinks**; keep LOCAL's backing native-clock-pristine. Drop-oldest drift
  on OBS/Discord-bound audio is fine.
- **Persistence:** extend `slopsmith-audio-settings.json` with a `buses[]` array (desired intent:
  `{name, taps, gains, sinkDevice/channels}`), re-established on startup like
  `reopenDesiredExtraInputs()`.

The three use cases map directly:
- **LOCAL** = `{ backing:1, sources:none, sink: primary ASIO/exclusive }` (player hears game; guitar
  via their rig; ~0 monitor latency).
- **STREAM A** = `{ backing:1, source: DI(in5)→NAM, sink: stream endpoint }`.
- **STREAM B** = `{ backing:1, source: wet(in1), sink: stream endpoint }`.

---

## The hard part: reaching OBS / Discord with NO external tools

**Universal limit (both OBS App-Audio Capture AND Discord Go-Live):** capture is **per-process** — it
grabs the *whole* FeedBack process audio, **merged**. You cannot isolate two mixes from one process
this way. → **Monitor-kill (PR #47) is the key lever:** make the process emit *only* the stream mix
while the monitor sits on **ASIO/exclusive** (invisible to WASAPI process-capture). Then app-capture /
Go-Live grabs exactly the intended mix, zero install.

### No-driver sink matrix (per OS)

| OS | Best no-driver path | Multiple independent mixes? | Notes |
|---|---|---|---|
| **Windows** | Route bus → a distinct **WASAPI endpoint** (spare interface out / 2nd interface / onboard); OBS *Audio Output Capture* or Discord grabs it. For one mix: **monitor-kill + app-capture/Go-Live**, zero install. | Only with **spare endpoints** (one per mix) | Extra **ASIO** channels are great for *hardware* routing but not software OBS (ASIO exclusive/single-client; `obs-asio` unmaintained on OBS 30+). |
| **macOS** | A cheap **user-space AudioServerPlugin** (BlackHole-class, notarized) → capture/Discord-mic. Or physical channels. | Yes (via the plugin / aggregate) | **Discord screen-share audio is still broken on macOS (2025)** → Mac leans on the virtual device. Cleanest *driver* story (no kernel, no BSOD). |
| **Linux** | **PipeWire null-sinks** at runtime → OBS PipeWire capture / Discord. | **Yes, full N-way, free** | Cleanest platform. Discord screen-share audio works via PipeWire/Pulse (Jan-2025 Wayland); `venmic` for PipeWire-direct. |

### The virtual-device question (a separate, deliberate decision)
A **shipped virtual audio device** is the only universal, no-spare-hardware way to N independent mixes.
But the **Windows** variant is a **signed kernel/APO driver** — EV cert (~$250/yr), Partner Center,
per-Windows-update re-validation, BSOD blast radius, **and anti-cheat (Vanguard/EAC) flags custom audio
drivers** → disqualifying as a default for a practice app. **macOS** AudioServerPlugin is cheap/safe;
**Linux** is free (null-sink). **Recommendation:** do **not** ship a Windows driver as the default —
either recommend an existing vetted cable, or treat the driver as its own scoped project. Ship the
no-driver `extraOutputs[]` path now.

---

## OBS vs Discord — two targets, one mechanism

| | **OBS → Twitch/YouTube** | **Discord (Go Live / Screen Share)** |
|---|---|---|
| Funnel role | **Acquisition** — public, indexed, clippable, social proof to strangers | **Activation / retention / social glue** — friends-only, two-click, high-frequency; warm word-of-mouth; on-ramp to public streaming |
| # mixes it can carry | **Multiple** (separate endpoints → OBS tracks, e.g. Stream A on Track 3, Stream B on Track 4) | **One combined mix** by nature (one call = one screen-share + one mic) |
| Best transport | Audio Output Capture of a routed endpoint (or app-capture + monitor-kill) | **Route A: Go Live "share application audio"** — bypasses Discord's voice DSP, stereo, clean |
| Fidelity | Platform-grade | **Opus voice-grade** — "jam-grade," not audiophile |

**Discord fast-follow specifics:**
- **Route A (recommended):** Go Live → share the FeedBack **application** audio. Bypasses Krisp/AEC/AGC,
  stereo, continuous. Windows ✓, macOS ✗ (screen-share audio broken), Linux ✓ (PipeWire/Pulse).
- **Route B (fallback, discouraged):** bus → virtual device → set as Discord **mic**. Runs music
  through **Krisp noise-suppression + AEC + AGC + VAD + VOIP-mode Opus + mono fold** — all of which
  **destroy music and are uncompensable from our side**. Only if not screen-sharing; requires a
  *blocking* "turn OFF Krisp/Noise-Suppression, Echo Cancellation, Automatic Gain" checklist.
- **Discord is one mix only** — the UI must not imply two simultaneous Discord mixes (single-choice
  tone radio, disable adding a second Discord bus).

---

## Latency & fidelity truths to SURFACE, not hide (sound-design)

- **The two guitars are the same performance at two delays.** The wet hardware tone (in1, ~0 latency
  to the player's ears) and the in-app DI→NAM re-amp (buffered) **must never share a bus** (comb/flange).
  **Every bus carries exactly ONE guitar; LOCAL carries ZERO** (verify no leak — strum hard, LOCAL
  meter must not move).
- **Stream buses must delay the GAME by Δ = L_in + L_mon** so the guitar locks to the game on the
  viewer's feed. **Δ is the same number the scorer already uses** — derive it from the source's
  verifier offset (`setVerifierUserOffset`), don't invent a new slider. Add the bus's own chain
  latency (needs a new `SignalChain` latency query; NAM≈0 today but a look-ahead VST would break it).
- **Every guitar-carrying bus needs a per-bus ZERO-LOOK-AHEAD limiter** at −1 to −1.5 dBTP (reuse
  `BackingLeveler`'s limiter stage, drop its AGC). Look-ahead would re-introduce the desync. The
  existing +6 dBFS sanitize scrub is *containment, not safety*.
- **Loudness:** OBS ~−16 LUFS / −1.5 dBTP. Discord lower & steadier (~−16 to −18, more steady-state
  compression, **mono-safe centered guitar**) — give Discord's normalizer nothing to chase.
- **Score off the DI (in5)**, not the distorted wet (in1).
- **Hearing-safety in a VC:** **headphones only** (speakers → howl loop, worse with AEC off); disable
  Discord join/leave + notification sounds (hard transients); never sum the VC return into the
  protected monitor pre-limiter.

---

## Marketing / growth framing (gamification)

- **Primary headline (community-native):** *"Press Go Live and your friends hear the game and your
  guitar — no setup, no extra apps."*
- **Secondary (creator tier):** *"Stream to Twitch with game + your tone in one screen — no VoiceMeeter."*
- **Audience tiers → the three mixes:** pro-rig (BYO wet tone), **mid-tier (re-amped DI = the volume
  unlock)**, beginner ("no amp? we'll re-amp you so your stream sounds great").
- **Why streamer-first:** for an instrument game, every stream is a playable demo + social proof +
  recruitment. Discord-direct is the **retention/social-glue + warm-lead** engine; OBS is **public
  acquisition**. The hard engine work is shared, so it's "and," not "or."
- **Anti-overscope:** the smallest thing that earns the headline = **one separated stream-mix bus**,
  monitor stays private. Defer multi-mix, ducking, overlays, clips.
- **Risks:** (1) **support burden** — audio setup is the #1 support sink, and "no external tools"
  makes echoes/wrong-device *our* bug → mandatory per-bus meter + "what OBS/Discord hears" preview;
  (2) **fidelity expectations** — Discord is Opus/"jam-grade," never market "audiophile over Discord";
  (3) **ToS/licensing optics** — making it frictionless to broadcast **bundled/charted copyrighted
  song audio** is the DMCA/Content-ID zone. **Default the stream mix to game-backing + the player's
  own playing; never market "stream your favorite songs."** Deserves Christian's counsel lens.

---

## Ranked PR plan

> Testing reality: native changes need a ~7-min desktop rebuild; this is a Windows box (can't run
> mac/Linux). The **routing/fan-out/teardown logic** gets deterministic C++/JS unit tests (the
> `tests/` harness already does this for chordscorer/multi-source without real devices); device I/O
> stays manual-verify (OBS/Discord capture + meters + soak). All PRs are `feedback-desktop` only.

**PR 1 — MVP: one configurable stream bus → chosen output. ← the single first PR.**
- Refactor the current single output into a reusable `OutputDeviceSlot` (sink 0) as the first commit
  (byte-identical), then add **one** extra sink + a minimal bus model (backing + a chosen, optionally
  NAM'd source + gain). Producer fans the selected sources+backing into the bus ring; bridge calls
  (`addOutputSink`/`setBusTaps`/`setBusGain`/`removeOutputSink`/`getBusMetrics`); persistence;
  **per-bus level + underflow meter**. Reject mismatched-SR sinks with a clear error.
- **Pairs with PR #47 (monitor-kill).** Delivers "game + my re-amped DI (or wet) → a chosen output,
  separate from what I monitor" — captured by **OBS Audio Output Capture** *or* **Discord Go Live**
  (validate the Discord Go-Live path first; it's the simplest, zero extra device on Windows).
- Risk: **medium** (new RT consumer + device lifecycle, but copied from the proven `extraInputs`).

**PR 2 — Full matrix: N buses → N sinks + routing UI.** Per-bus tap selection (card-stack UI primary,
optional 3×3 grid), delivers the OBS 3-way (LOCAL + A + B) simultaneously + **per-bus zero-look-ahead
limiter** + per-bus meters. Mostly UI; RT core proven by PR1.

**PR 3 — Robustness:** per-sink **async SRC** (mismatched rates), **auto-reopen** on mid-stream device
loss, "stream silent for N sec" warning, the `SignalChain` latency query for delay-comp.

**PR 4 (defer) — Polish:** presets ("Play for friends in Discord" / "Game only" / "Re-amp" / "My amp"),
the OBS + Discord setup helpers (literal step copy, "what they hear" preview, Krisp-off checklist on
the mic route), per-bus mute/solo, naming.

**Driver track (separate, deliberate):** macOS AudioServerPlugin (cheap, worth it) + Linux null-sink
(free); **no Windows kernel driver** (recommend an existing cable or scope it as its own project).
Owned by devops + Christian (cost/risk/anti-cheat).

---

## Decisions (LOCKED 2026-06-28)

1. **Backing render topology → SEPARATE copies.** Keep LOCAL backing native-clock-pristine on the
   player's own device; fan a *separate* backing render to the stream rings (drop-oldest drift on
   OBS/Discord-bound audio is fine). Monitor never inherits stream compromises; no double-advanced
   playhead.
2. **Same-interface routing → RING-FREE direct fan-out.** When buses share one interface (one clock),
   sum straight through — sample-accurate, lowest latency, no inter-bus ring. Rings only for
   genuinely separate devices / clocks.
3. **Discord surface → BOTH layers.** A destination *type* in the bus model (Discord is just another
   "send this mix to ___" target) AND a friendly "Play for friends in Discord" preset on top. Not two
   engines.
4. **Virtual audio device → MAC ONLY; NO Windows driver.**
   - **macOS:** ship our own user-space virtual device (BlackHole-class, notarized) as an **optional,
     substitutable** output — seamless out-of-box; user can disable it and use their own.
   - **Windows:** **do NOT build/ship a driver.** The default is no-cable: OBS App-Audio Capture /
     Discord Go-Live + **monitor-kill (#47)** → one stream mix, any interface, no spare output. Spare
     output / 2nd interface routes a mix for OBS (and separate A/B). The "separate mix with no spare
     output" niche uses the user's own free cable. A FeedBack-built Windows driver is rejected
     (signed system driver = signing cost + anti-cheat flags + BSOD risk + per-update maintenance).
   - **Reasoning correction baked in:** the Windows cable is niche because the **no-cable app-capture
     path covers the common single-mix case** — NOT because "most users have multi-out ASIO" (many are
     on 2-out interfaces with no spare). And note **spare ASIO outs can't cleanly feed OBS on the same
     PC** (ASIO exclusive/single-client; obs-asio dead on OBS 30+) — they feed *hardware* capture / a
     2nd PC, not same-machine OBS. So bring-your-own-cable everywhere works; we build only the Mac one.

---

## UI sketch (streaming-UX) — per-bus card stack (primary), grid on reveal

Lives in a **collapsed "Streaming & Extra Outputs"** section at the bottom of the Audio page (invisible
to the 95% who don't stream). A 3-question wizard forks first on **destination** (OBS / Discord / Both).
Buses shown as cards (🔵 "What you hear" / 🔴 "Stream → OBS" / 🟣 "Discord mix"), each with source
rows + level + a meter labeled *"this is exactly what OBS/Discord hears."* Presets cover the use cases
by name; an "Advanced routing" reveal exposes the full source×bus grid. Per-bus **OBS setup** / **Discord
setup** drawers print the literal capture steps for the route that bus targets, with a gentle test tone
+ mirrored meter as the "it's working" signal.
