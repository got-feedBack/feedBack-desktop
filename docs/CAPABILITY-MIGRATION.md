# Capability Migration Notes

Slopsmith Desktop is moving first-party renderer integrations away from private
webview globals and legacy `song:*` events toward Slopsmith capability domains.
The goal is to keep desktop behavior aligned with core while avoiding raw local
filenames, device handles, or native transport objects in plugin-visible state.

## Playback Identity For Tone Mappings

Core playback capability v1 emits redaction-safe lifecycle events such as
`playback:loading`, `playback:ready`, `playback:stopped`, and `playback:ended`.
The playback target contains two public identities:

- `targetId`: arrangement-scoped playback target identity.
- `settingsKey`: per-song storage identity shaped like `settings-v1-...`.

Desktop tone switching now uses `target.settingsKey` as the primary song key for
`localStorage.slopsmith-tone-mappings`. Raw filenames from `song:loading` remain
only as a compatibility fallback when the embedded Slopsmith core does not expose
playback capability v1.

### Storage Shape

The store shape does not change:

```json
{
  "global": {},
  "songs": {
    "settings-v1-abc1234": { "Clean": "Clean Preset" }
  },
  "midiPC": {
    "settings-v1-abc1234": { "mode": "midi", "vstSlotId": 0, "mappings": {} }
  }
}
```

Only the per-song bucket key changes. New mappings created on playback-capable
core builds are written under `settingsKey`. Existing filename-keyed buckets are
left in place so older core builds and older desktop releases can still read
them.

### Automatic Migration For Existing Mappings

When playback capability v1 emits `playback:loading`, desktop receives both the
safe `target.settingsKey` and the legacy filename fallback. If
`localStorage.slopsmith-tone-mappings` contains a filename-keyed `songs` or
`midiPC` bucket for the legacy filename and the corresponding `settingsKey`
bucket is missing or empty, desktop copies that bucket to the safe key and leaves
the original bucket untouched.

This migration is intentionally copy-only:

- Existing `settingsKey` buckets win and are not overwritten.
- Filename-keyed buckets remain available to older desktop builds or embedded
  Slopsmith cores without playback capability v1.
- Corrupt or missing mapping stores fall back to the normal empty-store behavior.

For review/debugging, open a song with an existing filename-keyed mapping and
inspect `localStorage.slopsmith-tone-mappings`: the same mapping should appear
under both the old filename key and the new `settings-v1-...` key after the
`playback:loading` event.

## Audio Effects Executor

Core `audio-effects` owns provider selection, policy, safe diagnostics, and the
`slopsmith.audio_effects.chain_plan.v1` schema. Desktop owns the trusted physical
executor. Renderer plugins may pass a core-resolved chain plan plus a private
trusted asset map to `window.feedBackDesktop.audioEffects.loadChainPlan(...)`;
desktop validates the schema, authorization, stage kinds, stage counts, opaque
asset references, local asset paths, and extension/kind compatibility before it
builds the native preset JSON and calls the existing native `loadPreset` path.

The preload surface is:

- `loadChainPlan(request)` — validates and loads a chain plan through the native
  engine. The request must include `authorization: "user-action"`,
  `"restore-selection"`, or `"playback-session"`.
- `inspectRoute(routeKey)` — returns a redaction-safe route summary: route key,
  provider id, plan id, state, stage-kind counts, active segment, and last
  outcome.
- `activateSegment(request)` — applies a segment by translating plan stage ids
  to loaded native slot ids and calling `setMultiBypass`.
- `setStageBypass(request)` and `setStageParameter(request)` — route stage-level
  controls to the loaded native slot.

The executor accepts raw paths only inside the trusted asset map passed to
desktop. It never echoes local paths, filenames, model names, IR names, VST state
blobs, native preset JSON, handles, callbacks, DOM nodes, audio buffers, samples,
or waveforms in its public outcomes. Failed rich-provider loads return structured
`failed`, `degraded`, `unavailable`, or `no-target` outcomes so NAM Tone or core can
fall back cleanly instead of leaving a partially described chain in public state.

### Maintainer Checklist

When migrating more desktop integrations to capabilities:

- Prefer capability events and snapshots over `window.playSong`, `window.stopSong`,
  and raw `song:*` events.
- Use `target.settingsKey` for local per-song plugin settings.
- Use `targetId` only for arrangement/session correlation, not persistent
  per-song settings.
- Route effect-chain execution through `window.feedBackDesktop.audioEffects`
  rather than passing raw native preset JSON through plugin-visible capability
  state.
- Keep raw filename fallback code behind a capability-version check.
- Add static migration guards under `tests/` for any removed global wrapper or
  new capability declaration.
