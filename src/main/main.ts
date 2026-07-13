// Slopsmith Desktop — Electron Main Process
// Manages: window lifecycle, Python subprocess, audio engine bridge, plugin management

// ── Velopack startup hook ─────────────────────────────────────────────────
// MUST run before ANY other side-effecting code (crashReporter, app event
// listeners, the rest of the imports below). When Windows invokes
// `Update.exe` with `--veloapp-install`/`--veloapp-updated`/`--veloapp-firstrun`
// it relaunches our exe with those flags; `VelopackApp.build().run()` is what
// detects them, runs the appropriate hook, and exits. If the hook doesn't
// run first the bootstrapper silently breaks install/upgrade flows.
// On macOS the hook just returns (no-op).
// Linux has no Velopack pipeline (electron-builder AppImage/deb only), so the
// native module is never needed there — skip the require entirely so loading
// it on an unsupported platform can never crash startup. On win/mac a load
// failure is also caught: a broken updater is recoverable, a dead app is not.
if (process.platform !== 'linux') {
    try {
        // eslint-disable-next-line @typescript-eslint/no-require-imports
        const { VelopackApp } = require('velopack') as typeof import('velopack');
        // eslint-disable-next-line @typescript-eslint/no-require-imports
        const { maybeUninstallLegacyNsis } = require('./nsis-migration') as typeof import('./nsis-migration');
        VelopackApp.build()
            // Fires during MSI install (the InstallHookDeferred custom
            // action in vpk's WiX template, post-InstallFiles). Detects a
            // legacy NSIS install at HKLM\...\Uninstall\Slopsmith, runs
            // its QuietUninstallString, and restores the Velopack stub
            // that NSIS deletes as part of its cleanup. No-op on machines
            // without the legacy install. 30-second hard budget — runs
            // synchronously with a sync registry-poll, well within it.
            .onAfterInstallFastCallback(() => {
                try {
                    maybeUninstallLegacyNsis();
                } catch (err) {
                    console.error('[main] NSIS cleanup hook failed:', err);
                }
            })
            .run();
    } catch (err) {
        // Never crash over this — a launchable app beats a dead one. But in a
        // packaged build a hook failure means install/update lifecycle flags
        // won't be handled, so surface it with a dialog instead of a console
        // line nobody reads. In dev/unpackaged builds the hook is a harmless
        // no-op, so a logged warning is enough there.
        console.error('[main] Velopack startup hook failed:', err);
        // eslint-disable-next-line @typescript-eslint/no-require-imports
        const { app, dialog } = require('electron');
        if (app.isPackaged) {
            dialog.showErrorBox(
                'fee[dB]ack update system error',
                'The Velopack updater failed to initialize. fee[dB]ack will still '
                + 'run, but automatic updates may not work until it is reinstalled.'
                + `\n\n${String(err)}`,
            );
        }
    }
}
// ──────────────────────────────────────────────────────────────────────────

import { app, BrowserWindow, ipcMain, dialog, shell, session, crashReporter, powerSaveBlocker, systemPreferences, desktopCapturer, screen } from 'electron';
import * as path from 'path';
import * as fs from 'fs';
import { execFileSync } from 'child_process';
import { migrateUserDataIfNeeded, consumePendingResetIfNeeded } from './config-bootstrap';

// Pin the userData folder name on every OS. Before this it was derived from the
// build name and differed per-OS ('fee[dB]ack' on macOS, 'slopsmith-desktop' on
// Linux/Windows), so a single "delete the config folder" instruction could never
// be right everywhere. setName() must run before ANY app.getPath()/crashReporter/
// whenReady so the deterministic path (<appData>/feedback-desktop) is used
// throughout. It does NOT change the user-facing brand (productName 'fee[dB]ack');
// with the name pinned, the path-hostile brackets never reach the filesystem.
app.setName('feedback-desktop');

// One-time copy of an existing legacy userData folder into the new one, so
// testers don't start fresh after the rename. MUST run before BOTH crashReporter
// (which creates <userData>/Crashpad) AND requestSingleInstanceLock() (which
// writes a SingletonLock into userData) — the migration gate is "new userData
// doesn't exist yet", so anything that creates it first would silently skip the
// migration and start the upgraded user fresh.
migrateUserDataIfNeeded();

// Acquire the single-instance lock now so the primary-only deferred-reset cleanup
// runs ONLY in the instance that will actually boot: a losing second instance
// must not consume the pending-reset manifest while the primary still holds
// Chromium / Crashpad files open. requestSingleInstanceLock() must be called
// exactly once; the lock branch at the bottom of this file reuses this result.
// SLOPSMITH_ALLOW_MULTIPLE=1 opts out (two builds side-by-side).
const allowMultipleInstances = process.env.SLOPSMITH_ALLOW_MULTIPLE === '1';
const hasSingleInstanceLock = allowMultipleInstances || app.requestSingleInstanceLock();
if (hasSingleInstanceLock) {
    // Apply any reset deletions deferred from a previous "Reset configuration"
    // run, before crashReporter / any BrowserWindow reopens Chromium state &
    // Crashpad, so those held-open paths can actually be removed.
    consumePendingResetIfNeeded();
}

// Enable Electron's Crashpad to capture native crashes (incl. VST/JUCE C++
// access violations) into <userData>/Crashpad/reports/ as .dmp files. Must
// run before app.whenReady(). uploadToServer:false keeps dumps local — they
// can be inspected with WinDbg / minidump-stackwalk.
crashReporter.start({
    productName: 'feedback-desktop',
    companyName: 'feedback',
    submitURL: '',
    uploadToServer: false,
    compress: false,
});
import { startPython, stopPython, waitForPython, getPythonPort, StartupStatus, restartPython, getLanUrls, getConfigDir } from './python';
import { runConfigMigrations } from './config-migrations';
import { registerMaintenanceHandlers } from './config-reset';
import {
    IPC_STARTUP_STATUS,
    IPC_STARTUP_GET_STATUS,
    IPC_STARTUP_REQUEST_STATUS,
    IPC_UPDATE_GET_STATUS,
    IPC_UPDATE_SET_CHANNEL,
    IPC_UPDATE_CHECK_NOW,
    IPC_UPDATE_APPLY,
    IPC_POWER_SET_SCREEN_AWAKE,
} from './ipc-channels';
import { initAudioBridge, shutdownAudio } from './audio-bridge';
import { initDebugLogging, isDebugEnabled } from './debug-log';
import { initPluginManager } from './plugin-manager';
import { initSoundfontManager, getDesktopConfig, setDesktopConfig } from './soundfont-manager';
import * as updateManager from './update-manager';
import type { UpdateChannel } from './update-manager';
import { installAppMenu } from './app-menu';
import { sanitizeWindowBounds, MIN_WIDTH, MIN_HEIGHT } from './window-bounds';
import {
    initPaneHosts, closeAllPanes, adoptPaneWindow, paneIdFromFrameName,
    togglePaneWindow, showAllPaneWindows, hideAllPaneWindows,
} from './pane-hosts';
import { initTray, destroyTray } from './pane-tray';

// Linux: enable Chromium's PipeWire capturer feature so getUserMedia can see
// audio devices on PipeWire-only distros (Fedora 36+, recent Ubuntu, Arch).
// Without this, Chromium falls back to PulseAudio enumeration, which on
// PipeWire systems sometimes returns an empty device list even when the JUCE
// engine sees the hardware fine. Must be set BEFORE app.whenReady() resolves —
// command-line switches are read during Chromium initialization.
//
// This is paired with the per-session permission handler installed in
// startup() below; together they unblock any renderer code that still calls
// navigator.mediaDevices.getUserMedia (the bundled note_detect plugin has
// since been routed through the JUCE bridge in
// slopsmith-plugin-notedetect#27, but third-party plugins may still hit the
// Web-Audio path on their own).
// Whether unprivileged user namespaces are usable — the sandbox path Chromium
// falls back to when the SUID helper is unavailable (as it always is from an
// AppImage). We don't guess purely from kernel knobs: AppArmor restriction
// (Ubuntu 24.04+) and other policy aren't fully reflected by sysctls. Instead we
// actually attempt to create a userns in a short-lived child — exactly what
// Chromium's sandbox does — and fall back to the knobs only when we can't probe.
function linuxUnprivilegedUsernsAvailable(): boolean {
    const readsZero = (p: string): boolean => {
        try { return fs.readFileSync(p, 'utf8').trim() === '0'; } catch { return false; }
    };
    // Cheap definite negatives — skip the probe when the kernel says no outright.
    if (readsZero('/proc/sys/kernel/unprivileged_userns_clone')) return false;
    if (readsZero('/proc/sys/user/max_user_namespaces')) return false;

    // Authoritative probe: try to create an unprivileged user namespace in a
    // short-lived child via util-linux `unshare`. Success → userns works for our
    // children, so Chromium can sandbox. A non-zero exit (EPERM, incl. AppArmor
    // denial) → unavailable. We never call unshare in-process, so the main
    // process is never moved into a namespace.
    try {
        execFileSync('unshare', ['--user', 'true'], { timeout: 2000, stdio: 'ignore' });
        return true;
    } catch (e) {
        // `unshare` binary not present → can't probe; fall back to the AppArmor
        // knob ('1' = restricted → treat as unavailable), else assume available.
        if ((e as NodeJS.ErrnoException)?.code === 'ENOENT') {
            try {
                return fs.readFileSync('/proc/sys/kernel/apparmor_restrict_unprivileged_userns', 'utf8').trim() !== '1';
            } catch { return true; }
        }
        // unshare ran but the namespace was denied (or timed out) → unavailable.
        return false;
    }
}

if (process.platform === 'linux') {
    // Merge with any existing `--enable-features=` value (set by Electron
    // defaults, parent env, or future code) instead of overwriting — a bare
    // appendSwitch would replace the comma-separated list and silently
    // disable everything else that was enabled. Split-and-dedupe so the
    // value stays stable across re-initializations (or if Chromium itself
    // already has WebRTCPipeWireCapturer in its baseline list).
    const existing = app.commandLine.getSwitchValue('enable-features');
    const features = new Set<string>(
        (existing || '').split(',').map((f) => f.trim()).filter(Boolean),
    );
    features.add('WebRTCPipeWireCapturer');
    app.commandLine.appendSwitch('enable-features', Array.from(features).join(','));

    // chrome-sandbox SUID abort fix (issue #438). From an AppImage the bundled
    // chrome-sandbox is inert — the squashfs is mounted nosuid so its SUID bit
    // can't take effect — and Chromium falls back to the unprivileged-userns
    // sandbox. On kernels where userns is disabled (Debian/Arch hardening) or
    // AppArmor-restricted (Ubuntu 24.04+), BOTH sandbox paths fail and Electron
    // ABORTS before any app code runs. We can't enable userns or make a nosuid
    // mount honor SUID, so on exactly that combination — running as an AppImage
    // AND no usable userns — fall back to --no-sandbox so the app launches at
    // all. The sandbox stays ON for .deb installs, dev runs, and every
    // userns-capable system; this only triggers where Electron would otherwise
    // die. Set before app.whenReady() so it's read during Chromium init.
    if (process.env.APPIMAGE && !linuxUnprivilegedUsernsAvailable()) {
        console.warn(
            '[startup] AppImage on a kernel without usable unprivileged user '
            + 'namespaces — launching with --no-sandbox so the app can start '
            + '(the Chromium process sandbox is disabled). To keep the sandbox, '
            + 'install the .deb, enable kernel.unprivileged_userns_clone=1, or '
            + 'run the AppImage on a userns-capable kernel. (issue #438)',
        );
        app.commandLine.appendSwitch('no-sandbox');
    }
}

if (process.platform === 'win32') {
    // Pin the WebGL/GPU process to the discrete (high-performance) adapter on
    // hybrid-GPU machines. On Windows laptops with an Intel iGPU + NVIDIA/AMD
    // dGPU, Chromium's GPU-process adapter selection is non-deterministic across
    // launches — one run binds the iGPU, the next the dGPU. When it lands on the
    // iGPU the 3D Highway's per-frame WebGL cost blows the draw budget and the
    // load-adaptive resolution scaler (feedBack#654, static/highway.js
    // _adaptRenderScale) silently drops the canvas to as low as quarter-res to
    // hold the frame rate — so the highway renders pixelated even with the
    // Quality selector pinned at HD, and which way it goes varies launch to
    // launch. The renderer's `powerPreference: 'high-performance'` WebGL hint
    // (plugins/highway_3d) is only advisory and does not reliably override the
    // OS/Chromium adapter choice. This switch forces the high-performance GPU at
    // the Chromium level so the fast path is consistent and the scaler rarely
    // engages. Set before app.whenReady() so it's read during Chromium init.
    // Desktop machines with a single GPU are unaffected (there's nothing to
    // pick); on dual-GPU desktops it likewise selects the discrete card.
    app.commandLine.appendSwitch('force_high_performance_gpu');
}

// Prevent error dialogs from showing when the Python subprocess has issues.
// Both handlers log and swallow — don't let a stray rejection in one of the
// subsystems tear the whole app down.
process.on('uncaughtException', (err) => {
    console.error('[main] Uncaught exception:', err.message);
});

process.on('unhandledRejection', (reason, promise) => {
    console.error('[main] Unhandled rejection at:', promise, 'reason:', reason);
});

let mainWindow: BrowserWindow | null = null;
let splashWindow: BrowserWindow | null = null;
// Set to true when the user initiates a quit so any in-flight startup work
// can bail out instead of racing the window teardown.
let appQuitting = false;
// Set to true the moment WE programmatically dismiss the splash (renderer
// painted, or the safety timeout fired). The splash 'close' handler treats a
// non-terminal close as a user-initiated quit; this flag lets it distinguish
// our intentional dismissal from the user hitting Alt+F4 on the splash.
let splashDismissing = false;
// Backstop timer: if the main window never finishes loading the renderer
// (did-finish-load on the server origin), close the splash and surface an
// error rather than leaving it spinning forever. The backend is already ready
// when we arm this (we're past core-ready), so the renderer only needs to
// fetch http://127.0.0.1:<port>/ and paint — a minute is very generous.
let splashSafetyTimer: ReturnType<typeof setTimeout> | null = null;
const SPLASH_RENDERER_DEADLINE_MS = 60_000;
// Milliseconds to keep the splash visible after a terminal error so the
// renderer has time to paint the final message before the window closes.
const SPLASH_CLOSE_DELAY_MS = 300;
let startupStatusSnapshot: StartupStatus = {
    running: true,
    phase: 'booting',
    message: 'Starting fee[dB]ack...',
    currentPlugin: '',
    loaded: 0,
    total: 0,
    error: null,
};


function getResourcesPath(): string {
    return app.isPackaged
        ? path.join(process.resourcesPath)
        : path.join(__dirname, '..', '..');
}

function publishStartupStatus(status: Partial<StartupStatus>): void {
    startupStatusSnapshot = { ...startupStatusSnapshot, ...status };
    if (splashWindow && !splashWindow.isDestroyed()) {
        splashWindow.webContents.send(IPC_STARTUP_STATUS, startupStatusSnapshot);
    }
    if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send(IPC_STARTUP_STATUS, startupStatusSnapshot);
    }
}

// Programmatically dismiss the splash. Idempotent. Sets splashDismissing so
// the 'close' handler below doesn't mistake this for a user-initiated quit,
// and clears the safety timer so it can't fire after we're done.
function dismissSplash(): void {
    splashDismissing = true;
    if (splashSafetyTimer) {
        clearTimeout(splashSafetyTimer);
        splashSafetyTimer = null;
    }
    if (splashWindow && !splashWindow.isDestroyed()) {
        splashWindow.close();
    }
}

// Terminal failure path: the renderer never painted, so the app is unusable.
// Two callers race here — the did-fail-load give-up branch (load retries
// exhausted) fires within ~30 s, while the splashSafetyTimer covers the case
// where the load silently hangs (server accepts the socket but never responds,
// so neither did-finish-load nor did-fail-load ever fires). Whichever fires
// first wins: dismissSplash() sets splashDismissing, so the same guard that
// makes dismissSplash idempotent short-circuits the second caller here too —
// no double error dialog. Surface the failure, tear the splash down, and quit.
function failRendererStartup(reason: string): void {
    if (appQuitting || splashDismissing) return;
    publishStartupStatus({ message: reason, phase: 'error', running: false });
    if (mainWindow && !mainWindow.isDestroyed()) {
        dialog.showErrorBox(
            'fee[dB]ack failed to start',
            'The app window did not finish loading. Please restart fee[dB]ack. '
            + 'If the problem persists, check the backend logs.',
        );
    }
    dismissSplash();
    app.quit();
}

function createSplashWindow(): void {
    splashWindow = new BrowserWindow({
        width: 560,
        height: 360,
        alwaysOnTop: true,
        resizable: false,
        minimizable: false,
        maximizable: false,
        fullscreenable: false,
        frame: false,
        show: true,
        title: 'fee[dB]ack',
        backgroundColor: '#050508',
        webPreferences: {
            preload: path.join(__dirname, 'splash-preload.js'),
            nodeIntegration: false,
            contextIsolation: true,
            // sandbox must be false so the preload script can require('electron')
            // (ipcRenderer). contextIsolation: true keeps the renderer isolated.
            sandbox: false,
        },
    });

    splashWindow.loadFile(path.join(__dirname, 'splash.html'));

    // Treat a user-initiated close (Alt+F4 / Cmd+W) before the renderer paints
    // as an explicit quit, so they are never stuck staring at the splash.
    // preventDefault() keeps the splash visible while app.quit() propagates
    // through before-quit → will-quit and tears the rest of startup down.
    splashWindow.on('close', (event) => {
        const currentPhase = startupStatusSnapshot.phase;
        // A user-initiated close (Alt+F4 / Cmd+W) before startup finishes is a
        // quit. But once the renderer has painted we dismiss the splash via
        // dismissSplash() while the backend is only at core-ready (plugins
        // still installing in the background, streamed to the renderer) — that
        // intentional dismissal sets splashDismissing and must NOT quit.
        if (!appQuitting && !splashDismissing && currentPhase !== 'complete' && currentPhase !== 'error') {
            event.preventDefault();
            app.quit();
        }
    });

    splashWindow.on('closed', () => {
        splashWindow = null;
    });
}

// Shared by the main BrowserWindow and any same-origin popup spawned via
// window.open (see setWindowOpenHandler / popupOverrideOptions below).
// Single source of truth so a future security-sensitive change (preload
// path, sandbox, webSecurity, isolation) can't update one path and leave
// the other diverged.
//   - sandbox: false is required for the preload to use require('electron').
//   - webSecurity: false lets the renderer load mixed-origin assets from
//     the localhost Python server.
const rendererWebPreferences: Electron.WebPreferences = {
    preload: path.join(__dirname, 'preload.js'),
    contextIsolation: true,
    nodeIntegration: false,
    sandbox: false,
    webSecurity: false,
};

// Off-origin sub-frame origins the renderer is allowed to embed. The
// will-frame-navigate guards below block every other off-origin sub-frame so
// a remote iframe can't ride the privileged preload, but the tutorials plugin
// legitimately embeds YouTube. This is safe only because preload.ts now gates
// its IPC bridge to the main frame — an allow-listed embed frame loads with no
// feedBackDesktop surface. Host-suffix match (exact host or `.`-prefixed
// sub-domain) so `evil-youtube.com` / `youtube.com.evil.com` don't slip past.
const EMBED_ALLOWED_HOSTS = ['youtube.com', 'youtube-nocookie.com'];
function isAllowedEmbedUrl(url: string): boolean {
    let host: string;
    try {
        const u = new URL(url);
        if (u.protocol !== 'https:') return false;
        host = u.hostname.toLowerCase();
    } catch {
        return false;
    }
    return EMBED_ALLOWED_HOSTS.some((h) => host === h || host.endsWith(`.${h}`));
}

// True for a host that resolves to this machine's loopback. Covers the whole
// 127.0.0.0/8 block, IPv6 loopback (bracketed or bare), IPv4-mapped-IPv6
// loopback, the unspecified address 0.0.0.0/:: (which routes to loopback on the
// local host), and localhost (with optional trailing dot). Used by the egress
// guard so an off-origin sub-frame can't dodge it with an aliased local host.
function isLoopbackHost(host: string): boolean {
    const h = host.toLowerCase().replace(/^\[|\]$/g, '');
    if (h === 'localhost' || h === 'localhost.') return true;
    if (h === '::1' || h === '::' || h === '0.0.0.0') return true;
    if (/^127\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(h)) return true;
    // IPv4-mapped IPv6, dotted tail: ::ffff:127.0.0.1
    const dotted = h.match(/^::ffff:(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})$/);
    if (dotted) return /^127\./.test(dotted[1]);
    // IPv4-mapped IPv6, hex tail (the form `new URL().hostname` normalizes to):
    // ::ffff:7f00:1 etc. Decode the embedded 32-bit IPv4 and test 127.0.0.0/8.
    const hex = h.match(/^::ffff:([0-9a-f]{1,4}):([0-9a-f]{1,4})$/);
    if (hex) {
        const v4 = ((parseInt(hex[1], 16) << 16) | parseInt(hex[2], 16)) >>> 0;
        return (v4 >>> 24) === 0x7f;
    }
    return false;
}

// True when `origin` is a concrete remote origin — a real scheme://host that is
// not our renderer. Opaque origins (a fresh about:blank/srcdoc frame reports
// "null") and the renderer origin itself return false, so this flags only a
// frame that has actually loaded a remote page (e.g. the YouTube embed).
function isConfirmedRemoteOrigin(origin: string, isRendererOrigin: (u: string) => boolean): boolean {
    if (!origin || origin === 'null') return false;
    if (isRendererOrigin(origin)) return false;
    try { new URL(origin); return true; } catch { return false; }
}

// True when `url` targets a local service the embed has no business reaching:
// any loopback host, OR our backend's own port on any host (the backend binds
// 0.0.0.0 in LAN mode, so it is also reachable on the machine's LAN IP — the
// port match catches that without us having to enumerate interfaces).
function isLocalServiceUrl(url: string, backendPort: number): boolean {
    try {
        const u = new URL(url);
        return isLoopbackHost(u.hostname) || u.port === String(backendPort);
    } catch {
        return false;
    }
}

function createWindow(port: number): void {
    // Restore the previous session's window geometry, validated against the
    // current display layout so stale bounds (unplugged monitor, resolution
    // change, hand-edited config) can't put the window off-screen. When x/y
    // are absent Electron centers the window as before.
    const restored = sanitizeWindowBounds(
        getDesktopConfig().windowBounds,
        screen.getAllDisplays().map((d) => d.workArea),
    );
    mainWindow = new BrowserWindow({
        x: restored.x,
        y: restored.y,
        width: restored.width,
        height: restored.height,
        minWidth: MIN_WIDTH,
        minHeight: MIN_HEIGHT,
        title: 'fee[dB]ack',
        backgroundColor: '#0f172a', // slate-900 to match Slopsmith UI
        webPreferences: rendererWebPreferences,
    });
    if (restored.maximized) mainWindow.maximize();

    // Persist geometry on close. getNormalBounds() so a maximized session
    // saves the underlying windowed size, restored + re-maximized next launch.
    // ponytail: fullscreen is deliberately not persisted (launching straight
    // into fullscreen is jarring, especially on macOS) and we save on close
    // only — a crash loses the last session's geometry; add debounced
    // resize/move saving if that ever matters.
    mainWindow.on('close', () => {
        if (!mainWindow || mainWindow.isDestroyed()) return;
        try {
            setDesktopConfig({
                windowBounds: { ...mainWindow.getNormalBounds(), maximized: mainWindow.isMaximized() },
            });
        } catch (err) {
            console.warn('[main] Failed to persist window bounds on close:', err);
        }
    });

    // Forward renderer console to main process stdout
    mainWindow.webContents.on('console-message', (_event, level, message, line, sourceId) => {
        const prefix = ['[renderer:verbose]', '[renderer:info]', '[renderer:warn]', '[renderer:error]'][level] || '[renderer]';
        console.log(`${prefix} ${message}`);
    });

    // Whole-app audio capture for exclusive-style outputs (ASIO / WASAPI
    // exclusive). The renderer-bus feeder in the static bundle calls
    // getDisplayMedia({audio, video}) to capture EVERY sound the app makes
    // (song, previews, UI) and push it into the engine's renderer bus —
    // per-surface taps can't cover plugin-private AudioContexts. Answer the
    // request with this window's own frame as the audio source (frame-scoped:
    // other applications' audio is NOT captured — a system 'loopback' would
    // leak Discord/etc. into the performance mix) and any screen as the
    // required-but-unused video track (the feeder stops it immediately).
    // [asio-diag] every outcome below is logged: a renderer-side
    // NotAllowedError with NO handler line in the main log means the running
    // main predates this handler (stale-main/new-bundle mix).
    session.defaultSession.setDisplayMediaRequestHandler((_request, callback) => {
        console.log('[asio-diag] display-media request received (loopback capture)');
        desktopCapturer.getSources({ types: ['screen'] }).then((sources) => {
            if (!mainWindow || sources.length === 0) {
                console.warn(`[asio-diag] display-media DENIED: mainWindow=${!!mainWindow} screenSources=${sources.length}`);
                callback({});
                return;
            }
            console.log('[asio-diag] display-media granted: frame audio + screen video');
            callback({ video: sources[0], audio: mainWindow.webContents.mainFrame });
        }).catch((e) => {
            console.warn(`[asio-diag] display-media DENIED: getSources failed: ${e?.message ?? e}`);
            callback({});
        });
    });

    // Local-mute companion for the capture above: when the feeder engages
    // loopback it must stop the page's audio from ALSO reaching the default
    // WASAPI device. Preferred path is the suppressLocalAudioPlayback track
    // constraint; this IPC is the fallback when the constraint is
    // unsupported. Chromium's capture pipeline taps frame audio before the
    // output mute, so a muted page still feeds the captured stream.
    ipcMain.handle('audio:setPageMuted', (_event, muted: unknown) => {
        if (!mainWindow) return false;
        mainWindow.webContents.setAudioMuted(muted === true);
        return mainWindow.webContents.isAudioMuted();
    });

    const serverUrl = `http://127.0.0.1:${port}`;

    // Clear the Chromium HTTP cache before the first load. The server
    // historically sent no Cache-Control on /static, so heuristic freshness
    // let a NEW build's window run the PREVIOUS build's app.js from disk
    // cache (2026-07-11 ASIO investigation: the whole exclusive-reroute chain
    // silently missing). The server now sends no-cache, but testers hop
    // between portable builds sharing one userData dir — one cheap clear per
    // launch makes stale-bundle states impossible regardless of what an
    // older build's server cached.
    const clearCachePromise = mainWindow.webContents.session.clearCache()
        .catch((e) => console.warn(`[main] clearCache failed (continuing): ${e.message}`));

    // Small delay to ensure server is fully accepting connections, then load
    setTimeout(() => { void clearCachePromise.then(() => mainWindow?.loadURL(serverUrl)); }, 500);

    // Retry loading if the server wasn't reachable yet. Previously this
    // retried just once, which left the window stuck on Chromium's
    // built-in error page when the python lifespan startup happened to
    // run long (or a zombie was holding the candidate port). Retry up
    // to maxRetries × intervalMs (~30 s total) with a fixed cadence —
    // long enough to ride out cold-cache plugin imports without giving
    // up. Only retry on the network-side error codes that indicate
    // "server not up yet"; don't retry on 404/etc. that mean the server
    // is up but served something we can't load.
    //   -102 = ERR_CONNECTION_REFUSED
    //   -6   = ERR_FILE_NOT_FOUND   (rare; shows up on transient races)
    //   -118 = ERR_CONNECTION_TIMED_OUT
    //   -2   = ERR_FAILED          (catch-all for transient socket errors)
    const retryableErrors = new Set([-102, -6, -118, -2]);
    const maxRetries = 20;
    const retryIntervalMs = 1500;
    let retryCount = 0;
    mainWindow.webContents.on('did-fail-load', (_event, errorCode) => {
        if (!retryableErrors.has(errorCode)) return;
        if (retryCount >= maxRetries) {
            console.log(`[main] gave up loading ${serverUrl} after ${maxRetries} retries (last errorCode=${errorCode})`);
            // Retries are exhausted — the renderer will never paint. Surface the
            // failure now rather than letting the splash spin until the safety
            // timer's full deadline (the give-up budget is well under it).
            failRendererStartup(`The app window failed to load (network error ${errorCode}). Please restart fee[dB]ack.`);
            return;
        }
        retryCount += 1;
        setTimeout(() => {
            if (!mainWindow) return;
            mainWindow.loadURL(serverUrl);
        }, retryIntervalMs);
    });

    // "Is this exactly the renderer origin we loaded?" — reused by the
    // did-finish-load paint check and the off-origin navigation guards below.
    // Matches scheme+host+port exactly, so a stray load of some *other*
    // loopback port can't be mistaken for our renderer.
    const isRendererOrigin = makeRendererOriginPredicate(port);

    // Inject mutable sync offset after page loads (default 200ms, overridden by settings).
    // Gate on the actual renderer origin — Chromium fires did-finish-load on its
    // built-in error pages too, and those have a null origin, so reading
    // localStorage from them throws SecurityError. Only inject when we
    // actually loaded the renderer origin.
    mainWindow.webContents.on('did-finish-load', () => {
        const url = mainWindow?.webContents.getURL() || '';
        // Chromium fires did-finish-load on its built-in error pages too (null
        // origin); only treat a load of the real renderer origin as "renderer
        // painted". This also gates the splash dismissal below — an error page
        // must NOT be taken as a successful paint.
        //
        // Note we deliberately do NOT distinguish a server-served 4xx/5xx body
        // here: those share the real renderer origin, carry no status on this
        // event, and aren't a realistic state anyway — waitForPython() already
        // confirmed backend health (an /api/plugins probe) before this window
        // was created, so a 5xx at / would be a fluke. Only the null-origin
        // Chromium error pages (and any stray load of another loopback port)
        // are filtered by the exact-origin check below.
        if (!isRendererOrigin(url)) return;
        mainWindow?.webContents.executeJavaScript(`
            window._slopsmithSyncOffset = parseFloat(localStorage.getItem('slopsmith-sync-offset') || '0.2');
        `).catch(() => {});
        // The renderer has painted, so the app is usable — dismiss the splash
        // even though plugins may still be installing in the background. Their
        // status streams to the renderer (SSE) and renders as disabled
        // "installing…" nav entries; the splash no longer waits on them (#421).
        dismissSplash();
    });

    // Block in-window navigation away from the renderer origin. The window
    // ships with `webSecurity: false` so the renderer can load
    // mixed-origin assets from the localhost server, but that same loose
    // setting means a stray click on a same-window link (or a 30x
    // redirect served through the local proxy) would still load a remote
    // page in our chrome — same preload, same exposed IPC. The permission
    // handler installed in startup() denies media/clipboard/etc for that
    // case, but it doesn't stop the navigation itself.
    //
    // Allow only navigations whose target matches the resolved renderer
    // origin. Anything else gets cancelled here and (via the
    // setWindowOpenHandler below) re-routed to the user's default browser
    // via shell.openExternal. (isRendererOrigin is derived once near the top
    // of createWindow above and reused here — the permission handler keeps its
    // own copy in a closure that isn't exposed.)
    // Block off-origin navigations at every layer Electron exposes:
    //
    // - `will-navigate`: user/script-initiated navigations on the main
    //   frame (link clicks, `location =`). Doesn't fire for
    //   programmatic loadURL — that's how we still let the initial
    //   load succeed.
    // - `will-redirect`: server-side 30x redirects during an
    //   in-progress navigation. The local proxy returning a 302 to a
    //   remote URL would otherwise bypass `will-navigate`.
    // - `will-frame-navigate`: navigations inside *any* frame
    //   (including the main frame). With `webSecurity: false` and a
    //   privileged preload running in every frame, an iframe loading
    //   a remote URL would inherit the IPC surface. We skip the main
    //   frame here so we don't double-process what `will-navigate`
    //   already handled.
    //
    // Electron 35 fires all three with a single `details` Event whose
    // `url` / `isMainFrame` are properties on the event, *not*
    // positional callback args. Reading them positionally returns
    // undefined and silently inverts the policy — `preventDefault()`
    // fires on every navigation including legitimate ones.
    function blockOffOriginTopLevel(reason: string) {
        return (details: Electron.Event<{ url: string }>) => {
            const navUrl = details.url;
            if (isRendererOrigin(navUrl)) return;
            details.preventDefault();
            console.warn(`[main] Blocked ${reason} to non-renderer origin: ${navUrl}`);
            // Only forward web URLs to the system browser. `file:`,
            // `javascript:`, `mailto:`, or custom schemes would
            // otherwise trigger the user's registered protocol handler
            // from a page-controlled string — a foot-gun even for a
            // navigation we're already blocking.
            openWebUrlExternally(navUrl);
        };
    }
    mainWindow.webContents.on('will-navigate', blockOffOriginTopLevel('in-window navigation'));
    mainWindow.webContents.on('will-redirect', blockOffOriginTopLevel('cross-origin redirect'));

    // A sub-frame that has loaded a remote page (the YouTube embed) must not
    // navigate itself — or submit a form — to a local service. Such a document
    // request would otherwise reach the backend: will-frame-navigate allows any
    // renderer-origin target, and the onBeforeRequest egress guard exempts
    // document loads. The onBeforeRequest guard only sees the per-resource
    // frame; here we have the frame's pre-navigation origin, which is the only
    // place we can tell "the YouTube frame is steering itself at 127.0.0.1" from
    // "a same-origin/fresh frame loads an app page". Returns true when blocked.
    function blockRemoteFrameToLocalService(details: Electron.Event<{ url: string; isMainFrame: boolean }>): boolean {
        const frame = (details as { frame?: Electron.WebFrameMain | null }).frame;
        if (!frame || !isLocalServiceUrl(details.url, port)) return false;
        // Walk the ancestor chain, not just this frame's origin: a remote embed
        // can spawn a FRESH child iframe (opaque origin) and steer THAT at the
        // backend. The child isn't a confirmed-remote origin itself, but its
        // parent (the embed) is — so any remote frame anywhere above this one
        // means a remote page is driving the navigation.
        let remoteAncestor: string | null = null;
        for (let f: Electron.WebFrameMain | null = frame; f; f = f.parent) {
            if (isConfirmedRemoteOrigin(f.origin, isRendererOrigin)) { remoteAncestor = f.origin; break; }
        }
        if (!remoteAncestor) return false;
        details.preventDefault();
        console.warn(`[main] Blocked remote frame ${remoteAncestor} from navigating to local service: ${details.url}`);
        return true;
    }

    mainWindow.webContents.on('will-frame-navigate', (details) => {
        // Top-level frame is handled by will-navigate above; skip so
        // we don't double-log or route to openExternal twice.
        if (details.isMainFrame) return;
        if (blockRemoteFrameToLocalService(details)) return;
        const navUrl = details.url;
        if (isRendererOrigin(navUrl)) return;
        // Allow trusted media embeds (e.g. the tutorials plugin's YouTube
        // player). Safe because the preload bridge is main-frame-only, so the
        // embed frame carries no privileged IPC surface.
        if (isAllowedEmbedUrl(navUrl)) return;
        details.preventDefault();
        // Don't openExternal subframe blocks — popping the system
        // browser every time an embedded video / ad-frame tries to
        // load is worse UX than silently refusing.
        console.warn(`[main] Blocked subframe navigation to non-renderer origin: ${navUrl}`);
    });

    // window.open() routing:
    //
    // - Same renderer-origin URLs → allow as an Electron BrowserWindow
    //   that mirrors the main window's webPreferences (same preload,
    //   same isolation, same webSecurity: false). Plugin pop-outs like
    //   splitscreen rely on this so the popup shares the renderer's
    //   BroadcastChannel scope and preload-exposed IPC. Without it,
    //   `action: 'deny'` returns null to window.open() (which the
    //   plugin reads as "popup blocked") AND the URL leaks to the
    //   system browser via openWebUrlExternally, where BroadcastChannel
    //   can't reach across Chromium instances.
    //
    // - Off-origin URLs → route to the system browser. Same scheme
    //   gate as will-navigate above (openWebUrlExternally restricts to
    //   http/https) since a target=_blank or stray window.open from a
    //   plugin can supply any string.
    const popupOverrideOptions: Electron.BrowserWindowConstructorOptions = {
        backgroundColor: '#0f172a',
        webPreferences: rendererWebPreferences,
    };
    const rendererWindowOpenHandler = ({ url }: { url: string }) => {
        if (isRendererOrigin(url)) {
            return { action: 'allow' as const, overrideBrowserWindowOptions: popupOverrideOptions };
        }
        openWebUrlExternally(url);
        return { action: 'deny' as const };
    };
    mainWindow.webContents.setWindowOpenHandler(rendererWindowOpenHandler);

    // Apply the same off-origin navigation guards + window-open policy
    // to any popup the renderer opens. Otherwise a popup could be told
    // to navigate off-origin (or spawn another window.open), and the
    // preload-IPC surface installed via popupOverrideOptions would
    // follow along to the new page.
    //
    // Wired recursively: the last line re-registers `did-create-window`
    // on each popup's own webContents so nested popups (popup A →
    // popup B) inherit the same guards. Without that, only popups
    // spawned directly from the main window would be protected, and
    // any popup that spawned another would leave its child guard-less
    // while still carrying the preload-IPC surface.
    function wirePopupGuards(wc: Electron.WebContents): void {
        wc.on('will-navigate', blockOffOriginTopLevel('popup in-window navigation'));
        wc.on('will-redirect', blockOffOriginTopLevel('popup cross-origin redirect'));
        wc.on('will-frame-navigate', (details) => {
            if (details.isMainFrame) return;
            if (blockRemoteFrameToLocalService(details)) return;
            const navUrl = details.url;
            if (isRendererOrigin(navUrl)) return;
            if (isAllowedEmbedUrl(navUrl)) return;
            details.preventDefault();
            console.warn(`[main] Blocked popup subframe navigation to non-renderer origin: ${navUrl}`);
        });
        wc.setWindowOpenHandler(rendererWindowOpenHandler);
        wc.on('did-create-window', (nestedWin) => wirePopupGuards(nestedWin.webContents));
    }
    mainWindow.webContents.on('did-create-window', (popupWin, details) => {
        wirePopupGuards(popupWin.webContents);
        // A pane pop-out. The RENDERER opened it (window.open) because it moves a
        // live DOM node into it and needs a handle on the new document to do that —
        // see pane-hosts.ts. We recognise it by the frame name it was opened with
        // and give it the OS behaviour a pane should have: remembered bounds, off
        // the taskbar, minimize-to-tray, listed in the tray menu.
        const paneId = paneIdFromFrameName(details.frameName || '');
        if (paneId) adoptPaneWindow(popupWin, paneId);
    });

    mainWindow.on('closed', () => {
        mainWindow = null;
        // Pane windows cannot outlive the window that feeds them: without the
        // renderer there is nothing on the other end of their BroadcastChannel,
        // so they would sit there showing a frozen playhead forever. Worse, a
        // pane HIDDEN in the tray is still an open window — leaving one behind
        // would stop `window-all-closed` from ever firing and the app would
        // linger as an invisible process.
        closeAllPanes();
    });

    // Dev tools in development
    if (!app.isPackaged) {
        mainWindow.webContents.openDevTools({ mode: 'detach' });
    }
}

// Forward a URL to the OS default browser only if it's a web URL.
// `shell.openExternal` will gladly hand any string to the user's
// registered protocol handlers (file:, javascript:, mailto:, custom
// schemes), and the strings we pass through come from page-controlled
// places (will-navigate, window.open). Restrict to http(s) so a
// malformed link, plugin bug, or attacker-shaped string can't reach
// for arbitrary scheme handlers.
function openWebUrlExternally(url: string): void {
    let parsed: URL;
    try { parsed = new URL(url); } catch {
        console.warn(`[main] Refusing to openExternal malformed URL: ${url}`);
        return;
    }
    if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') {
        console.warn(`[main] Refusing to openExternal non-web scheme: ${parsed.protocol}`);
        return;
    }
    // Pass the canonicalised href, not the raw input — page-controlled
    // strings can carry whitespace / control characters that the URL
    // parser strips, and openExternal should see exactly the bytes we
    // validated.
    shell.openExternal(parsed.href).catch(() => { /* user dismissed / system error */ });
}

// Permissions we always deny, regardless of origin. These are
// high-impact device APIs Slopsmith has no use for — refusing them up
// front shrinks the attack surface even on the trusted renderer
// origin, so a malicious or compromised plugin can't reach for them.
// Add to this list if a real Slopsmith feature ever needs one.
const DENY_PERMISSIONS = new Set([
    'serial',
    'hid',
    'usb',
    'bluetooth',
    'geolocation',
    'idle-detection',
]);

// Predicate factory: did a permission request originate from the exact
// origin we load the renderer from? The window's `loadURL` target is
// `http://127.0.0.1:${port}` (see createWindow), so the trusted origin
// is precisely that protocol + hostname + port triple — no wider
// hostname allow-list. `http://localhost:${port}` could resolve to a
// different listener on a dual-stack host and must NOT inherit the
// grant.
//
// Parses via WHATWG URL so query strings, fragments, and other
// valid-but-uncommon URL shapes don't trip the check. URL.origin
// canonicalises to `scheme://host:port`, which is exactly the equality
// we want — anything else (different port, different scheme, different
// hostname, malformed URL) compares unequal.
function makeRendererOriginPredicate(rendererPort: number): (url: string) => boolean {
    const expectedOrigin = `http://127.0.0.1:${rendererPort}`;
    return (url: string): boolean => {
        if (!url) return false;
        try {
            return new URL(url).origin === expectedOrigin;
        } catch {
            return false;
        }
    };
}

// Origin- AND port-scoped permission policy for the default session.
//
// Why this exists: clicking Detect in the bundled note_detect plugin on
// Linux used to show "Could not access audio input" (#52) because Chromium
// in Electron silently denies `media` for the localhost-served renderer
// when no permission handler is installed. The plugin itself now routes
// pitch detection through the JUCE bridge, but we still want a defensive
// handler so future renderer code / third-party plugins don't hit the
// same wall. createWindow() also installs a `will-navigate` listener that
// cancels any same-window navigation off the renderer origin, so the
// scenarios this handler defends against are mostly belt-and-braces;
// `webSecurity: false` is still on, so a defense-in-depth permission
// policy is worth keeping anyway.
//
// Policy:
// - Block DENY_PERMISSIONS (serial / hid / usb / bluetooth / geolocation /
//   idle-detection) for *every* origin, including the renderer. Slopsmith
//   has no use for these; pre-denying them keeps a compromised plugin
//   from reaching for them.
// - For `media` from the renderer origin, allow audio-only requests
//   (Slopsmith uses the microphone for pitch detection) but deny when
//   `details.mediaTypes` includes `video` — we have no camera feature,
//   so a getUserMedia({video:true}) call must be a plugin bug or worse.
// - For every other permission, grant when the request comes from the
//   exact rendererPort we resolved at startup. This matches Electron's
//   prior default-allow for the only origin we actually load, so
//   unrelated renderer/plugin features (clipboard, notifications,
//   fullscreen, midi, …) keep working unchanged.
// - For any other origin (including other ports on 127.0.0.1, redirects
//   to external URLs, etc.), deny. Stops a stray redirect from
//   inheriting clipboard / notifications / etc. that would have been
//   default-allowed without a handler.
function installRendererPermissions(rendererPort: number): void {
    const isRendererOrigin = makeRendererOriginPredicate(rendererPort);
    const def = session.defaultSession;
    def.setPermissionRequestHandler((_wc, permission, callback, details) => {
        // [asio-diag] getDisplayMedia (loopback capture) rides this path
        // ('display-capture' and/or 'media' with video) BEFORE the
        // display-media handler — log every deny so an upstream permission
        // denial is distinguishable from a handler-level one.
        const deny = (why: string) => {
            console.warn(`[asio-diag] permission-request DENIED: ${permission} (${why}) url=${details.requestingUrl ?? ''}`);
            callback(false);
        };
        if (DENY_PERMISSIONS.has(permission)) {
            deny('deny-list');
            return;
        }
        if (!isRendererOrigin(details.requestingUrl || '')) {
            deny('non-renderer origin');
            return;
        }
        if (permission === 'media') {
            // Electron passes mediaTypes (`'audio'` / `'video'`) for
            // getUserMedia requests. Allow explicit audio-only (microphone
            // for pitch detection) and deny camera. getDisplayMedia — the
            // renderer-bus whole-app loopback capture — ALSO rides this
            // permission but with EMPTY mediaTypes, so empty must be
            // allowed or the display-media handler below never runs and
            // exclusive/ASIO output loses all page audio (song previews,
            // fallback element songs). Verified 2026-07-12: the old
            // "empty ⇒ deny" rule was exactly the tester's silent-preview
            // bug. Camera exposure stays impossible: getDisplayMedia video
            // is the app's own frame, and real camera requests always
            // carry mediaTypes=['video'].
            const mediaTypes = (details as { mediaTypes?: string[] }).mediaTypes;
            const types = new Set(mediaTypes ?? []);
            if (types.has('video')) { deny(`media types=[${[...types].join(',')}]`); return; }
            callback(true);
            return;
        }
        callback(true);
    });
    def.setPermissionCheckHandler((_wc, permission, requestingOrigin, details) => {
        if (DENY_PERMISSIONS.has(permission)) {
            console.warn(`[asio-diag] permission-check DENIED: ${permission} (deny-list) origin=${requestingOrigin ?? ''}`);
            return false;
        }
        if (!isRendererOrigin(requestingOrigin || '')) {
            console.warn(`[asio-diag] permission-check DENIED: ${permission} (non-renderer origin) origin=${requestingOrigin ?? ''}`);
            return false;
        }
        if (permission === 'media') {
            // Mirror the request-handler in the synchronous check path so
            // navigator.permissions.query() reports the same state we'll
            // actually grant: deny camera ('video'), allow audio and the
            // type-less display-capture path. Electron's check-handler
            // `details` exposes `mediaType` (singular), unlike the
            // request-handler's `mediaTypes` (plural array).
            const mediaType = (details as { mediaType?: string }).mediaType;
            if (mediaType === 'video') {
                console.warn(`[asio-diag] permission-check DENIED: media (mediaType=video) origin=${requestingOrigin ?? ''}`);
                return false;
            }
            return true;
        }
        return true;
    });

    // Network egress guard for the off-origin embeds the will-frame-navigate
    // allow-list now permits (the tutorials YouTube player). The window runs
    // `webSecurity: false`, so a remote sub-frame could otherwise use plain
    // fetch / XHR / WebSocket to reach the local backend. CORS still blocks it
    // from *reading* any response (verified), but a state-changing GET /
    // simple-POST would still land (blind CSRF), and an Origin-header check
    // misses `no-cors` requests (Origin: null). So gate on the initiating frame:
    // only the main frame and same-origin sub-frames may reach a local service.
    def.webRequest.onBeforeRequest((details, callback) => {
        if (!isLocalServiceUrl(details.url, rendererPort)) {
            callback({});
            return;
        }
        // Document navigations (the renderer's own load, sub-frame src loads)
        // are policed by createWindow()'s navigation guards — never cancel them
        // here, or we'd block the app's own page load (which has no frame yet).
        if (details.resourceType === 'mainFrame' || details.resourceType === 'subFrame') {
            callback({});
            return;
        }
        // Data requests (xhr/fetch/websocket/…). Allow the main frame and any
        // same-origin sub-frame (origin, not url, so inherited-origin about:blank
        // / srcdoc frames still count). Deny an off-origin sub-frame (the embed)
        // and — fail closed — any request whose frame was torn down mid-flight to
        // shed attribution. Slopsmith uses no service/shared workers, so no
        // legitimate local-service request is frameless.
        const frame = details.frame;
        if (frame && (frame.parent === null || isRendererOrigin(frame.origin))) {
            callback({});
            return;
        }
        console.warn(`[main] Blocked local-service request from ${frame ? `off-origin subframe ${frame.url}` : 'unattributed frame'} → ${details.url}`);
        callback({ cancel: true });
    });
}

// macOS microphone (TCC) gate. The JUCE engine captures guitar input through
// a native CoreAudio input device (AudioEngine opens it during audio.init()),
// NOT through the renderer's getUserMedia — so the Chromium media-permission
// flow (installRendererPermissions / setPermissionRequestHandler) never fires
// for it. On macOS a native CoreAudio input open is gated by TCC, and unless
// the *main app process* has explicitly asked for microphone access the open
// is silently denied: no prompt, a dead input gauge, and — because the engine
// couples the input and output device open — no audio output either. That's
// the reported "no sound and never asks for mic permission" bug; users worked
// around it by launching the binary from Terminal, which makes Terminal the
// responsible TCC process and forces the prompt.
//
// Requesting access here, from the bundle that actually carries
// NSMicrophoneUsageDescription (resources entitlements + extendInfo), shows the
// system prompt once and registers Slopsmith itself in System Settings →
// Privacy & Security → Microphone. Must run BEFORE initAudioBridge() so the
// grant is in place before the native engine opens the input device. No-op off
// macOS (Windows/Linux don't gate capture this way), and best-effort: a denial
// or a throw must not block startup — the engine still runs output-only and the
// renderer surfaces the missing-input state through the normal device UI.
async function ensureMicrophoneAccess(): Promise<void> {
    if (process.platform !== 'darwin') {
        console.log('[main] ensureMicrophoneAccess: platform !== darwin, skipping');
        return;
    }
    // Only run in a packaged app. NSMicrophoneUsageDescription is injected by
    // electron-builder via extendInfo in package.json and is only present in
    // the built .app bundle. Calling askForMediaAccess() without that Info.plist
    // key in an unpackaged dev run (npm start / plain Electron.app) terminates
    // the process instead of throwing — the try/catch does not catch it.
    if (!app.isPackaged) {
        console.log('[main] ensureMicrophoneAccess: app.isPackaged === false, skipping (NSMicrophoneUsageDescription may be absent)');
        return;
    }
    try {
        const status = systemPreferences.getMediaAccessStatus('microphone');
        console.log(`[main] ensureMicrophoneAccess: getMediaAccessStatus returned '${status}'`);

        if (status === 'granted') {
            // TCC database has a grant for this bundle ID, BUT macOS enforces
            // the grant against the code signature — a stale entry (keyed to a
            // different signature from an earlier build) reports 'granted' here
            // while CoreAudio still delivers zeroed input. Log bundle and
            // packaging details so the debug log can diagnose signature-mismatch
            // vs. genuine grant.
            const name = app.getName();
            const ver = app.getVersion();
            const pkg = app.isPackaged;
            const resPath = app.getPath('exe');
            console.log(`[main] Microphone access: 'granted' (early return — app="${name}" v${ver} packaged=${pkg} exe="${resPath}")`);
            console.log('[main] ensureMicrophoneAccess: returning early — status is granted, no askForMediaAccess call');
            return;
        }

        if (status === 'denied' || status === 'restricted') {
            // Already a hard 'denied'/'restricted' verdict — askForMediaAccess
            // resolves false without re-prompting. Surface it so the log
            // explains a silent input; the user must re-enable via System
            // Settings (or `tccutil reset Microphone`).
            console.warn(`[main] Microphone access is '${status}'; the OS will not re-prompt. ` +
                'Enable fee[dB]ack under System Settings → Privacy & Security → Microphone.');
            return;
        }

        // status === 'not-determined' → this triggers the one-time OS prompt.
        console.log('[main] Microphone access: status is not-determined, calling askForMediaAccess (system prompt should appear)');
        const granted = await systemPreferences.askForMediaAccess('microphone');
        console.log(`[main] Microphone access ${granted ? 'granted' : 'denied'} by user.`);

        if (!granted) {
            // User denied the prompt — log extra context so we can distinguish
            // "user clicked Don't Allow" from "prompt never appeared / resolved false".
            console.warn('[main] Microphone access: user denied the permission prompt. Input will be silent.');
        }
    } catch (e: unknown) {
        const msg = e instanceof Error ? e.message : String(e);
        console.warn(`[main] Microphone access request failed: ${msg}`);
        // Log the full error stack if available — the try/catch above may not
        // catch a process termination (missing NSMicrophoneUsageDescription),
        // but anything that does reach here should be diagnosable.
        if (e instanceof Error && e.stack) {
            console.warn(`[main] Microphone access error stack: ${e.stack}`);
        }
    }
}

async function startup(): Promise<void> {
    // Debug logging first so everything below is captured. SLOPSMITH_SANDBOX_DEBUG
    // gates the addon's VST_TRACE — flip it whenever debug is *requested*, even
    // if the log file couldn't be opened, so addon tracing isn't silently lost.
    // The addon caches the var on first read, so it must be set before
    // initAudioBridge() loads the .node.
    const debugLogPath = initDebugLogging();
    if (isDebugEnabled()) {
        process.env.SLOPSMITH_SANDBOX_DEBUG = '1';
    }
    if (debugLogPath) {
        console.log(`[main] Debug logging enabled → ${debugLogPath}`);
    }

    console.log('[main] Starting Slopsmith Desktop...');

    // Register startup status IPC handlers before creating the splash window
    // so the splash preload's immediate startup:requestStatus is handled.
    ipcMain.handle(IPC_STARTUP_GET_STATUS, () => startupStatusSnapshot);
    ipcMain.on(IPC_STARTUP_REQUEST_STATUS, (event) => {
        event.sender.send(IPC_STARTUP_STATUS, startupStatusSnapshot);
    });

    createSplashWindow();
    publishStartupStatus({ message: 'Starting backend service...', phase: 'booting', running: true });

    // Run config-schema migrations against the active backend CONFIG_DIR before
    // the backend starts. This replaces "delete the config folder before
    // upgrading" with targeted, idempotent, fail-soft migrations. Logging the
    // resolved CONFIG_DIR here also closes the visibility gap around the silent
    // Linux ~/.local/share/slopsmith shared-config override (python.ts getConfigDir).
    try {
        const activeConfigDir = getConfigDir();
        console.log(`[main] Active CONFIG_DIR: ${activeConfigDir}`);
        runConfigMigrations(activeConfigDir, app.getVersion(), new Date().toISOString());
    } catch (err) {
        console.warn('[main] config migrations failed (continuing):', err);
    }

    // Register the "Reset / repair configuration" IPC handlers (Settings panel).
    // Pass the window getter so the destructive-reset confirmation is a native,
    // main-process modal (the renderer bridge is reachable by plugin scripts, so
    // a renderer-only confirm is not a sufficient gate).
    registerMaintenanceHandlers(() => mainWindow);

    // Start Python server (Slopsmith backend)
    startPython();

    // Prompt for macOS microphone access before the engine opens its native
    // CoreAudio input device (see ensureMicrophoneAccess). Awaited so the TCC
    // grant lands before initAudioBridge() → audio.init() touches the input.
    await ensureMicrophoneAccess();

    // Initialize audio engine (JUCE native addon).
    initAudioBridge();

    // Initialize plugin manager IPC handlers
    initPluginManager();

    // Initialize soundfont manager IPC handlers (Audio Quality preference)
    initSoundfontManager(() => mainWindow);

    // Wait for Python server to be ready; null means the backend failed to start.
    const port = await waitForPython().catch((err: unknown) => {
        const message = err instanceof Error ? err.message : String(err);
        console.error('[main] Backend failed to start:', message);
        publishStartupStatus({ message: `Backend failed to start: ${message}`, phase: 'error', running: false });
        return null;
    });

    if (port === null) {
        await new Promise((resolve) => setTimeout(resolve, SPLASH_CLOSE_DELAY_MS));
        if (splashWindow && !splashWindow.isDestroyed()) splashWindow.close();
        app.quit();
        return;
    }

    console.log(`[main] Python server ready on port ${port}`);
    publishStartupStatus({ message: 'Backend ready. Opening app window...', phase: 'core-ready', running: true });

    // Permission handlers must be installed before the renderer loads so
    // its first permission request hits our policy, not Chromium's
    // default. We deferred until now because the policy is scoped to the
    // exact renderer port — a stray navigation to another local service
    // on a different port must not inherit the trusted-renderer grant.
    installRendererPermissions(port);

    // Create the main window
    createWindow(port);

    // Detachable panes: the tray that lists them, and the OS behaviour applied to
    // each pane window as the renderer opens it (see did-create-window above).
    // Must come after createWindow — both reach the renderer through mainWindow,
    // and Tray requires a ready app.
    initPaneHosts({ getMainWindow: () => mainWindow });
    // The tray's pane actions are INJECTED, not imported: pane-hosts already imports
    // pane-tray (to push the menu), and importing back would make the two modules
    // mutually dependent — a require cycle whose loser sees half-initialised exports.
    initTray({
        getMainWindow: () => mainWindow,
        toggleWindow: togglePaneWindow,
        showAll: showAllPaneWindows,
        hideAll: hideAllPaneWindows,
    });

    // Install our application menu (replaces Electron's default so View →
    // Zoom In also accepts the unshifted Ctrl+= key — see app-menu.ts).
    installAppMenu();

    // Register file picker IPC
    ipcMain.handle('dialog:pickFile', async (_event, filters?: { name: string; extensions: string[] }[]) => {
        if (!mainWindow) return null;
        const result = await dialog.showOpenDialog(mainWindow, {
            properties: ['openFile'],
            filters: filters || [{ name: 'All Files', extensions: ['*'] }],
        });
        return result.canceled ? null : result.filePaths[0];
    });

    ipcMain.handle('dialog:pickDirectory', async () => {
        if (!mainWindow) return null;
        const result = await dialog.showOpenDialog(mainWindow, {
            properties: ['openDirectory'],
        });
        return result.canceled ? null : result.filePaths[0];
    });

    ipcMain.handle('dialog:pickFiles', async (_event, filters?: { name: string; extensions: string[] }[]) => {
        if (!mainWindow) return [];
        const result = await dialog.showOpenDialog(mainWindow, {
            properties: ['openFile', 'multiSelections'],
            filters: filters || [{ name: 'All Files', extensions: ['*'] }],
        });
        return result.canceled ? [] : result.filePaths;
    });

    // App info
    ipcMain.handle('app:getInfo', () => ({
        version: app.getVersion(),
        isPackaged: app.isPackaged,
        platform: process.platform,
        resourcesPath: getResourcesPath(),
    }));

    // Config directory
    ipcMain.handle('app:getConfigDir', () => {
        return app.getPath('userData');
    });

    // LAN access — opt-in toggle to bind the backend to 0.0.0.0 so other
    // devices on the network can reach the library / sync room (issue #441).
    // Default is loopback-only; enabling restarts the Python backend so the
    // new bind address takes effect on the same port.
    ipcMain.handle('network:getLanAccess', () => ({
        enabled: !!getDesktopConfig().lanAccess,
        urls: getLanUrls(),
    }));
    ipcMain.handle('network:setLanAccess', async (_event, enabled: unknown) => {
        const on = enabled === true;
        setDesktopConfig({ lanAccess: on });
        // Re-spawn uvicorn with the new --host. Same port is reused (the old
        // process releases it first), so the already-loaded 127.0.0.1
        // renderer keeps working once the backend is back up.
        restartPython();
        // Wait for the backend to actually rebind before resolving, so the UI
        // doesn't hand out a LAN URL that 404s during the ~restart window. If
        // it doesn't come back in time we still report the intended state —
        // the renderer surfaces failures through the normal startup path.
        try {
            await waitForPython();
        } catch {
            /* backend slow/failed to restart — return intended state anyway */
        }
        return { success: true, enabled: on, urls: getLanUrls() };
    });

    // Auto-update (Velopack). The renderer Settings panel reads the persisted
    // channel from localStorage and calls setChannel() on boot — we default
    // to 'stable' here so the first check runs against the safest feed even
    // if the renderer hasn't paged in yet. On Linux every call short-circuits
    // to { status: "unsupported", platform: "linux" } inside update-manager.
    ipcMain.handle(IPC_UPDATE_GET_STATUS, () => updateManager.getStatus());
    ipcMain.handle(IPC_UPDATE_SET_CHANNEL, (_event, channel: unknown) => {
        // IPC is untyped at runtime — validate the channel string before forwarding
        // so a renderer bug or compromised page can't pass arbitrary values into
        // the Velopack SDK.
        const VALID_CHANNELS: readonly string[] = ['stable', 'rc', 'beta', 'alpha', 'nightly'];
        if (typeof channel !== 'string' || !VALID_CHANNELS.includes(channel)) {
            return updateManager.getStatus();
        }
        updateManager.setChannel(channel as UpdateChannel);
        return updateManager.getStatus();
    });
    ipcMain.handle(IPC_UPDATE_CHECK_NOW, () => updateManager.checkNow());
    ipcMain.handle(IPC_UPDATE_APPLY, () => updateManager.applyAndRestart());

    // Keep the display awake while a song plays (got-feedback/feedback#686). The
    // renderer toggles this via window.feedBackDesktop.power.setScreenAwake on
    // play/pause; the single OS blocker is refcounted across renderers below.
    ipcMain.handle(IPC_POWER_SET_SCREEN_AWAKE, (event, keep: unknown) => {
        setRendererScreenAwake(event.sender, keep === true);
    });

    // Boot the updater after the main window exists so the first
    // update:available / update:downloaded broadcast has a renderer to land
    // in. Renderer will call setChannel() once it reads localStorage.
    updateManager.init('stable');

    // The splash now dismisses on the main window's did-finish-load (renderer
    // painted) — see createWindow(). We no longer poll /api/startup-status to
    // gate the splash on full plugin load: plugin status streams straight to
    // the renderer over SSE and renders incrementally (#421). Arm a backstop
    // so a renderer that never paints still tears the splash down and surfaces
    // an error instead of spinning forever. (Exhausted load retries surface
    // sooner, from the did-fail-load give-up branch in createWindow().)
    splashSafetyTimer = setTimeout(() => {
        splashSafetyTimer = null;
        // Backstop for a silently hung load (no did-finish-load AND no
        // did-fail-load — e.g. the socket is accepted but the response never
        // arrives). The retry-exhaustion case surfaces earlier via the
        // did-fail-load give-up branch; failRendererStartup() is idempotent so
        // whichever path fires first wins.
        failRendererStartup('The app window failed to load. Please restart fee[dB]ack.');
    }, SPLASH_RENDERER_DEADLINE_MS);
}

// ── Single-instance lock ──────────────────────────────────────────────────
// Without this, each launch spins up its own Python backend (fighting for the
// same fixed port) and its own GPU context; stacking instances starves the GPU
// and was observed to stutter the 3D highway. Hold a process-wide lock: if
// another instance already owns it, surface that window (via 'second-instance'
// on the primary) and quit THIS one before startup() boots a competing backend
// or window. `SLOPSMITH_ALLOW_MULTIPLE=1` opts out so two builds can run
// side-by-side (e.g. A/B testing different versions). The lock was already
// acquired at the top of this file (hasSingleInstanceLock) so primary-only reset
// side effects could run before crashReporter — reuse that result here rather
// than calling requestSingleInstanceLock() a second time.
if (!hasSingleInstanceLock) {
    app.quit();
} else {
    if (!allowMultipleInstances) {
        app.on('second-instance', () => {
            // A second launch was attempted — focus the existing window
            // instead of opening another. mainWindow may not exist yet if
            // we're still on the splash, so fall back to it.
            const win = mainWindow ?? splashWindow;
            if (win && !win.isDestroyed()) {
                if (win.isMinimized()) win.restore();
                win.show();
                win.focus();
            }
        });
    }
    app.whenReady().then(startup);
}

app.on('window-all-closed', () => {
    shutdown();
    app.quit();
});

app.on('before-quit', () => {
    appQuitting = true;
    shutdown();
});

// Screen wake lock (got-feedback/feedback#686). The single OS powerSaveBlocker is
// held while at least one renderer wants the screen awake (a song is playing in
// it). Renderers are refcounted in a set so a multi-window setup (the main
// window plus a same-origin popout) stays correct — one window pausing must not
// drop the blocker while another is still playing.
let powerBlockerId: number | null = null;
const powerAwakeRenderers = new Set<Electron.WebContents>();
const powerCleanupWired = new WeakSet<Electron.WebContents>();

function syncPowerBlocker(): void {
    if (powerAwakeRenderers.size > 0) {
        if (powerBlockerId === null || !powerSaveBlocker.isStarted(powerBlockerId)) {
            powerBlockerId = powerSaveBlocker.start('prevent-display-sleep');
        }
    } else if (powerBlockerId !== null) {
        if (powerSaveBlocker.isStarted(powerBlockerId)) powerSaveBlocker.stop(powerBlockerId);
        powerBlockerId = null;
    }
}

function setRendererScreenAwake(wc: Electron.WebContents, keep: boolean): void {
    if (keep) {
        powerAwakeRenderers.add(wc);
        // Drop this renderer's hold if it reloads, crashes, or its window closes
        // before sending setScreenAwake(false), so the blocker can't outlive its
        // playback. Listeners are persistent (.on) so repeated reloads/crashes on
        // the same WebContents keep cleaning up; wired once per WebContents so
        // play/pause cycles don't pile up duplicates.
        if (!powerCleanupWired.has(wc)) {
            powerCleanupWired.add(wc);
            const drop = () => { powerAwakeRenderers.delete(wc); syncPowerBlocker(); };
            wc.on('did-start-loading', drop); // reload / navigation
            wc.on('render-process-gone', drop); // renderer crash (may recur)
            wc.on('destroyed', drop); // window closed
        }
    } else {
        powerAwakeRenderers.delete(wc);
    }
    syncPowerBlocker();
}

function shutdown(): void {
    try {
        console.log('[main] Shutting down...');
    } catch { /* console may already be gone mid-teardown */ }
    destroyTray();
    powerAwakeRenderers.clear();
    syncPowerBlocker();
    updateManager.shutdown();
    shutdownAudio();
    // shutdown() only runs on app quit (window-all-closed / before-quit), and
    // the main process exits the moment this synchronous chain returns — so
    // stop the backend SYNCHRONOUSLY (SIGKILL the group) rather than relying on
    // stopPython()'s async force-kill timer, which would never fire and would
    // leave a slow-to-exit uvicorn (graceful shutdown blocking on an in-flight
    // scan / conversion / demucs job) orphaned, holding its ML-model RAM.
    stopPython(/* immediate */ true);
}

// macOS: re-create window when dock icon is clicked
app.on('activate', async () => {
    if (BrowserWindow.getAllWindows().length === 0) {
        const port = getPythonPort();
        if (port > 0) createWindow(port);
    }
});
