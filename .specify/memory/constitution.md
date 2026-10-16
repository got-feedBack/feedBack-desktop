# Slopsmith Desktop Constitution

## Core Principles

### I. Wrap, Don't Fork

The desktop app is a *shell* around upstream Slopsmith. It MUST clone
or bundle Slopsmith verbatim and run its `server.py` as a subprocess.
We MUST NOT fork the Slopsmith codebase or carry private patches: if
the desktop needs new behaviour from Slopsmith, the change goes
upstream (gated by an env var or feature flag) before landing here.

### II. Native Audio Is the Whole Point

The reason this app exists (vs. running Slopsmith in a browser) is
low-latency audio with VST/AU/LV2 hosting, NAM, IRs, and pitch
detection. The audio engine MUST be a JUCE C++ native addon
(`src/audio/`) compiled via cmake-js, exposed to Electron through
N-API (`src/audio/NodeAddon.cpp`) and bridged into the renderer via
`src/main/audio-bridge.ts`. Audio code MUST NOT live in JS/TS.

### III. Three-Process Architecture

The runtime has exactly three layers:

1. **Renderer** (Slopsmith UI in a webview, talks to localhost
   Python and to Electron via preload IPC).
2. **Electron main** (`src/main/`) — window lifecycle, IPC, plugin
   manager, Python supervisor, audio bridge.
3. **Python subprocess** (`src/main/python.ts` spawning
   `slopsmith/server.py` on port 18000+).

New features MUST fit one of these layers. Audio engine work is
loaded from layer 2 via the native addon and MUST NOT bypass it.

### IV. Bundle Everything Required to Run Offline

A first-run install MUST work without an internet connection: the
Python interpreter, Slopsmith source tree, default IRs, and the
default soundfont MUST be packaged via `electron-builder`'s
`extraResources`. Plugins are the exception (installed on demand
into the user's config dir).

### V. Reproducible Linux Builds Via DevContainer

Linux distribution builds MUST be reproducible against the
`ubuntu-22.04` GitHub Actions image. The DevContainer
(`.devcontainer/`) and `scripts/build-linux-docker.sh` are the
canonical build path; ad-hoc `npm run dist:linux` on a developer's
host is for development iteration only.

### VI. Plugin Isolation From the App

Plugins MUST live in the user's config directory
(`~/.config/slopsmith-desktop/plugins/`) and be managed via
`src/main/plugin-manager.ts`. Updating the desktop app MUST NOT
touch installed plugins; removing the desktop app MUST NOT delete
them by default. Plugins are git clones of public repos; the plugin
manager wraps `git`.

### VII. Fail Soft on Audio Engine Absence

If the native audio addon fails to load (missing build, missing
runtime libs, etc.), the rest of the app MUST keep working —
Slopsmith UI, plugins, library browsing, all still functional.
`src/main/audio-bridge.ts` already swallows load failures and
returns `audio:isAvailable === false`; new features MUST follow the
same pattern.

### VIII. Cross-Platform Means All Three (NON-NEGOTIABLE)

We support Windows 10+, macOS 12+, and Linux. Every feature MUST
ship working code paths for all three. Per-OS audio backends:
ASIO/CoreAudio/JACK+ALSA. VST3 everywhere; AU on macOS; LV2 on
Linux. Per-OS distribution targets: AppImage+deb, dmg+zip, NSIS exe.

## Operational Constraints

- **Stack**: Electron + TypeScript (main/renderer wiring), JUCE C++
  (audio engine), Python 3.12 (Slopsmith subprocess), CMake 3.22+.
- **Native Modules**: `slopsmith_audio.node` built via cmake-js,
  unpacked from asar at runtime (`asarUnpack` in
  `package.json.build`).
- **Resources**: `resources/{slopsmith,python,bin,default-irs,
  soundfonts}` are extra-resourced into the packaged app.
- **Server port range**: 18000+ (chosen to avoid colliding with a
  Docker Slopsmith on 8000 — see `src/main/python.ts`).
- **App ID**: `com.byron.slopsmith-desktop`.
- **NAM**: NeuralAmpModelerCore vendored at
  `src/audio/third_party/NAM/`; NAM support is conditional on the
  vendored tree being present (CMakeLists.txt).

## Development Workflow

- `npm run dev` — TS rebuild + Electron launch (uses the system
  Python, not the bundled one).
- `npm run build:audio` — JUCE native addon (Release).
- `npm run dist:{linux,mac,win}` — full bundle + electron-builder.
- New audio capabilities go through `SignalChain` and a new
  `*Processor` C++ class; the addon API is in
  `src/audio/NodeAddon.cpp`.
- Plugin manager UI changes go in `src/renderer/plugin-manager/`.
- Splash screen / startup polling lives in `src/main/main.ts` and
  `src/main/splash*.ts`; respect the 5-minute startup deadline and
  the 700 ms poll interval already documented there.

## Governance

This repo is one of several in the Slopsmith ecosystem. The shared
workspace at `~/Repositories/slopsmith-workspace/` coordinates
cross-repo work. Constitution amendments here MUST consider whether
the upstream Slopsmith repo needs a parallel change (Principle I).
The desktop app is a downstream consumer of upstream Slopsmith,
upstream `slopsmith-demucs-server` (optional, configured by the
user), and the per-feature plugin repos (optional, installed by the
user).

**Version**: 1.0.0 | **Ratified**: 2026-05-09 | **Last Amended**: 2026-05-09
