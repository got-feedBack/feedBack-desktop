# feedBack Desktop

The desktop app for [feedBack](https://github.com/got-feedBack/feedBack).
It is an Electron shell that runs the feedBack core server with a bundled Python runtime and adds a native JUCE audio engine for low-latency instrument input.

To play, download a build from [Releases](https://github.com/got-feedBack/feedBack-desktop/releases).
This README is for developers.

## Prerequisites

- Node.js and npm (`.build-config.json` pins Node 22)
- Python 3.12
- CMake and a C++ toolchain (Xcode Command Line Tools on macOS, see [WINDOWS_BUILD_REQUIREMENTS.md](WINDOWS_BUILD_REQUIREMENTS.md) on Windows)
- git
- `ffmpeg` and `vgmstream-cli` on your `PATH`
- On Linux, the ALSA, JACK, freetype2 and X11 dev headers

`bash scripts/setup-dev.sh` checks that the commands are on your `PATH`, plus the Linux dev headers and the macOS Command Line Tools, and prints what is missing.
It does not check the Node.js or Python versions, or the Windows C++ toolchain, so confirm those yourself.
It also runs the submodule and `npm install` steps below.

## Dev setup

1. Clone this repo with its submodules (JUCE, RTNeural, NAM, signalsmith):

   ```bash
   git clone --recurse-submodules https://github.com/got-feedBack/feedBack-desktop.git
   cd feedBack-desktop
   ```

   In an existing clone, run `git submodule update --init --recursive`.

2. Get a checkout of the core server and point `SLOPSMITH_DIR` at it:

   ```bash
   git clone https://github.com/got-feedBack/feedBack.git ../feedBack
   export SLOPSMITH_DIR="$PWD/../feedBack"
   ```

   If `SLOPSMITH_DIR` is unset, dev mode looks for `../slopsmith`, then `~/Repositories/slopsmith`.
   On Windows, use a native path such as `C:\src\feedBack`, not a Git Bash path like `/c/src/feedBack`.

3. Install the core's Python dependencies (Linux and macOS shown here).
   Dev mode uses `.venv/bin/python3` in this repo if it exists, otherwise `python3` from your `PATH`:

   ```bash
   python3 -m venv .venv
   .venv/bin/pip install -r "$SLOPSMITH_DIR/requirements.txt"
   ```

4. Install npm dependencies and build the native audio engine:

   ```bash
   npm install
   npm run build:audio
   ```

   Use `npm run build:audio:debug` for a debug build, or `npm run rebuild:audio` to rebuild from scratch.

5. Start the app:

   ```bash
   npm start
   ```

   This compiles the TypeScript (`npm run build:ts`) and launches Electron.

## Checks

```bash
npm run typecheck
npm test
```

## Packaging

`./scripts/build-release.sh` builds a release for the host OS.
On Ubuntu it runs `build-linux-ubuntu.sh`, on other Linux distros `build-linux-docker.sh` (the same build inside Docker), on macOS `build-macos.sh` and on Windows `build-windows.sh`.
You can also run `./scripts/build-linux-docker.sh` directly for a reproducible Linux build.

`npm run dist:linux` runs electron-builder directly, after building the native addon and bundling the server, Python and binaries.
`npm run dist:mac` and `npm run dist:win` do not bundle Python or the binaries, so use the platform scripts for those.

More detail:

- [docs/BUILD_ARCHITECTURE.md](docs/BUILD_ARCHITECTURE.md): how the build scripts, config and CI fit together
- [scripts/BUILD_SCRIPTS.md](scripts/BUILD_SCRIPTS.md): the per-platform build scripts
- [.devcontainer/README.md](.devcontainer/README.md): the CI-identical Linux build container
- [WINDOWS_BUILD_REQUIREMENTS.md](WINDOWS_BUILD_REQUIREMENTS.md): Windows toolchain

## Contributing

See the [org contributing guide](https://github.com/got-feedBack/.github/blob/main/CONTRIBUTING.md).
Report bugs in the [feedBack issue tracker](https://github.com/got-feedBack/feedBack/issues).

## License

[AGPL-3.0-only](LICENSE). Bundled third-party components are listed in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
