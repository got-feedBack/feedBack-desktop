// Auto-updater for fee[dB]ack Desktop. Windows + macOS go through Velopack;
// Linux (AppImage only) goes through a home-grown GitHub-releases checker,
// since Velopack has no Linux support.
//
// Architecture (Windows/macOS — Velopack):
//   - The renderer persists the user's release channel in localStorage and
//     calls setChannel() on boot so this module's UpdateManager is bound to
//     the right feed (stable | rc | beta | alpha | nightly). The nightly feed
//     is published by .github/workflows/nightly.yml as a rolling `nightly`
//     GitHub Release (rid channels win-x64-nightly / osx-arm64-nightly),
//     unlike the tag-driven alpha/beta/rc/stable feeds from build.yml.
//   - On init() and then every 4 hours we run checkForUpdatesAsync(); when a
//     hit comes back we download in the background, broadcast
//     update:available immediately and update:downloaded once the .nupkg is
//     on disk. The renderer shows a banner whose "Restart to apply" button
//     funnels back into applyAndRestart().
//
// Architecture (Linux — AppImage):
//   - electron-builder ships the nightly Linux build as a plain AppImage
//     (release/*.AppImage), uploaded to the same rolling `nightly` GitHub
//     Release as the Velopack win/mac feed — but with no Velopack manifest,
//     so there's nothing for the Velopack SDK to read on this platform.
//   - Only the `nightly` channel is supported (stable/rc/beta/alpha are
//     one-off tagged releases, not a rolling release, so "find the newest
//     matching tag" isn't a single API call the way `releases/tags/nightly`
//     is — not needed for the current Linux use case, so left unsupported).
//   - The AppImage filename and app.getVersion() never change between
//     nightly builds (only win/mac get a date-stamped Velopack version), so
//     semver comparison can't detect a new nightly. Instead the build bakes
//     its source commit into dist/main/build-info.json (see build-common.sh);
//     we compare that baked SHA against the GitHub release's
//     `target_commitish` (the commit the latest nightly was cut from). A
//     mismatch means the running build differs from the published nightly →
//     offer the update. This needs no persistent state: after a swap +
//     relaunch the new AppImage carries its OWN baked SHA, which now matches
//     the release, so the next check reports idle. A short-lived in-memory
//     note (linuxDownloadedSha) stops the 4h poll from re-downloading a
//     build we've already staged this session.
//   - On a SHA mismatch we download the `*.AppImage` asset next to the
//     running AppImage (process.env.APPIMAGE, set by the AppImage runtime)
//     and rename it over the original — same filesystem, so the rename is
//     atomic, and Linux allows replacing a file that's currently executing
//     (the running process keeps its old inode). No separate "apply" step is
//     needed for the file swap; applyAndRestart() just relaunches the
//     (already-replaced) AppImage and quits this process.
//   - If the AppImage isn't running as an AppImage (process.env.APPIMAGE
//     unset — e.g. a .deb install or an unpackaged dev build) or the channel
//     isn't `nightly`, every method reports { status: "unsupported" }.
//
// Velopack JS SDK API notes (verified against
//   node_modules/velopack/lib/index.d.ts, package 0.0.1589-ga2c5a97,
//   and the matching native source at src/lib-rust/src/sources/{mod,github}.rs
//   in github.com/velopack/velopack):
//   - There is **no** `GithubSource` class exported from the JS package
//     (unlike the .NET SDK). `UpdateManager`'s constructor takes a plain
//     `urlOrPath: string` — pointing it at the GitHub repo's HTML URL is
//     enough because the Velopack server-side metadata (`releases.<ch>.json`
//     uploaded by `vpk pack`) lives in the release assets and Velopack's
//     native loader knows how to pull them via the GitHub Releases API.
//   - `UpdateOptions` exposes `AllowVersionDowngrade` (plan called it
//     `AllowDowngrade`; the actual key in the typings is the longer name) +
//     `ExplicitChannel` (matches the plan).
//   - **`UpdateOptions` does NOT expose a `Prerelease` field**, and the
//     native `AutoSource::new` for any github.com URL hardcodes
//     `GithubSource::new(input, None, false)`. Result: this SDK cannot
//     see GitHub releases marked `prerelease: true`. The CI release
//     job therefore publishes every tag (incl. alpha/beta/rc) as
//     non-prerelease — see the long comment in .github/workflows/build.yml
//     above the `Create Release` step. Channel scoping is unaffected
//     because Velopack picks the release by channel-manifest filename
//     (releases.<channel>.json), not by GitHub prerelease flag.

import { app, BrowserWindow } from 'electron';
import { spawn } from 'child_process';
import * as fs from 'fs';
import * as path from 'path';
import { Readable, Transform } from 'stream';
import { pipeline } from 'stream/promises';
import type { UpdateInfo } from 'velopack';
import { IPC_UPDATE_EVENT_AVAILABLE, IPC_UPDATE_EVENT_DOWNLOADED, IPC_UPDATE_EVENT_PROGRESS, IPC_UPDATE_EVENT_DIAG } from './ipc-channels';
import { linuxUpdateDecision } from './linux-update-decision';

// Abort the small metadata request if GitHub stalls, so a dead connection
// surfaces as an error instead of a frozen "checking" state forever. The
// large AppImage download deliberately has no such deadline — it can legitimately
// run for minutes; its progress broadcasts are what reveal a stall there.
const METADATA_FETCH_TIMEOUT_MS = 30_000;

export type UpdateChannel = 'stable' | 'rc' | 'beta' | 'alpha' | 'nightly';

export type UpdateStatus =
    | { status: 'unsupported'; platform: 'linux' }
    | { status: 'idle'; channel: UpdateChannel; currentVersion: string | null; lastChecked: number | null }
    | { status: 'checking'; channel: UpdateChannel; currentVersion: string | null; lastChecked: number | null }
    | { status: 'downloading'; channel: UpdateChannel; currentVersion: string | null; lastChecked: number | null; pending: { version: string }; percent: number | null }
    | { status: 'downloaded'; channel: UpdateChannel; currentVersion: string | null; lastChecked: number | null; pending: { version: string } }
    | { status: 'error'; channel: UpdateChannel; currentVersion: string | null; lastChecked: number | null; message: string };

// Repo the Velopack feed (win/mac) and the Linux nightly release both live
// in. Matches the existing electron-builder release pipeline.
const FEED_URL = 'https://github.com/got-feedback/feedback-desktop';

// GitHub REST API URL for the rolling nightly release. Public repo, no auth
// needed. Only used on Linux.
const GITHUB_NIGHTLY_RELEASE_API = FEED_URL.replace('https://github.com/', 'https://api.github.com/repos/') + '/releases/tags/nightly';

// Background poll cadence. Cheap on both platforms (Velopack HEADs the
// channel manifest; the Linux path is a single small GitHub API GET), so 4h
// is a reasonable trade-off between freshness and noise on the user's network.
const POLL_INTERVAL_MS = 4 * 60 * 60 * 1000;

// Held in module scope (singleton) — `main.ts` calls `init()` once after
// `app.whenReady()` resolves and never reconstructs us.
let velopackUm: import('velopack').UpdateManager | null = null;
let currentChannel: UpdateChannel = 'stable';
let pollTimer: NodeJS.Timeout | null = null;
let initialCheckTimer: NodeJS.Timeout | null = null;
let inFlightCheck: Promise<UpdateInfo | null> | null = null;
let linuxInFlightCheck: Promise<UpdateStatus> | null = null;
// The remote nightly SHA we've already downloaded + staged THIS session, so a
// later poll (remote unchanged) doesn't re-fetch the ~1.5GB AppImage. Not
// persisted: after a restart the swapped binary's own baked SHA matches the
// release and reports idle. Survives channel round-trips (stable↔nightly) so
// the staged-update UI is restored when returning to nightly.
let linuxDownloadedSha: string | null = null;
// The background AppImage download promise (null when none running) + the
// latest whole-percent progress, so getStatus() can report progress to a
// renderer that (re)loads the panel mid-download, not just live listeners.
let linuxDownloadInFlight: Promise<void> | null = null;
let downloadPercent: number | null = null;
// Generation counter: incremented every time setChannel() replaces velopackUm
// so that in-flight checks from the old channel can detect they are stale and
// skip all state mutations + broadcasts. Without this, a check running on the
// old manager's promise would still write activeState/pendingDownloaded and
// broadcast update:available/downloaded after the channel switch.
let checkGeneration = 0;
let lastChecked: number | null = null;
let pendingVersion: string | null = null;   // set as soon as a target version is known (download starting)
let pendingDownloaded: { version: string } | null = null;  // set after download completes
let lastError: string | null = null;
let activeState: 'idle' | 'checking' | 'downloading' | 'downloaded' | 'error' = 'idle';

const isLinux = process.platform === 'linux';

function broadcast(channel: string, payload: unknown): void {
    for (const win of BrowserWindow.getAllWindows()) {
        if (!win.isDestroyed()) {
            win.webContents.send(channel, payload);
        }
    }
}

// Trace of main-process update decisions (Linux path only — this is the
// half of the story invisible to the renderer's diagnostics.js console wrap).
// Broadcasting it to the renderer means it lands in the SAME exportable
// console ring buffer the renderer's own [update-diag] logs use, so a single
// "Export Diagnostics" click captures both sides. Also console.error()s for
// visibility when launched from a terminal. Never allowed to affect the
// actual update flow — swallow any broadcast failure.
function diagLog(message: string, data?: Record<string, unknown>): void {
    console.error(`[update-diag] ${message}`, data ?? '');
    try {
        broadcast(IPC_UPDATE_EVENT_DIAG, { ts: Date.now(), message, data: data ?? null });
    } catch {
        // ignore
    }
}

function currentVersion(): string | null {
    if (isLinux) return app.getVersion();
    if (!velopackUm) return null;
    try {
        return velopackUm.getCurrentVersion();
    } catch {
        // Velopack throws when run from an unpackaged build (no manifest on
        // disk). That's a normal dev-loop state — surface null rather than
        // letting the throw bubble up into the IPC layer.
        return null;
    }
}

// Velopack requires every unique os/rid to have its own channel when one
// feed (a single GitHub release) serves multiple platforms — otherwise the
// per-channel `releases.<channel>.json` manifests collide as release
// assets. We ship x64-only Windows and arm64-only macOS, so the rid prefix
// is fixed per platform. `vpk pack` in CI publishes manifests under these
// exact names (win-x64-<track> / osx-arm64-<track>).
function veloChannel(track: UpdateChannel): string {
    // The rid is derived from BOTH platform and arch: an x64 (Intel) vs
    // arm64 macOS build must query its own channel manifest. Hardcoding
    // osx-arm64 would point an Intel build at the wrong (incompatible) feed.
    const os = process.platform === 'win32' ? 'win' : 'osx';
    const arch = process.arch === 'arm64' ? 'arm64' : 'x64';
    return `${os}-${arch}-${track}`;
}

function createManager(channel: UpdateChannel): void {
    // main.ts runs the Velopack startup hook (require('velopack') +
    // VelopackApp.build().run()) on win/mac before anything else. This lazy
    // require is a second layer of safety: a constructor-level failure here
    // (bad options, corrupted state dir) — or a velopack load failure that
    // main.ts caught and logged rather than crashed on — is surfaced as the
    // 'error' state by init()/setChannel() instead of crashing the process.
    // createManager() is only ever reached on win/mac (init()/setChannel()
    // route to the Linux path first), so the require is safe to run here.
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const { UpdateManager } = require('velopack') as typeof import('velopack');
    velopackUm = new UpdateManager(FEED_URL, {
        ExplicitChannel: veloChannel(channel),
        AllowVersionDowngrade: false,
        MaximumDeltasBeforeFallback: 10,
    });
}

// ── Linux (AppImage) update path ────────────────────────────────────────

type GithubReleaseAsset = { name: string; browser_download_url: string };
type GithubRelease = { target_commitish: string; assets: GithubReleaseAsset[] };

export type BuildInfo = { sha: string | null; coreSha: string | null };

let cachedBuildInfo: BuildInfo | null = null;

// The commit(s) this build was cut from, baked into the packaged app by
// build-common.sh (dist/main/build-info.json, alongside the compiled JS):
// `sha` is feedback-desktop's own commit, `coreSha` is the bundled core
// (feedBack) repo's commit at clone time. Both null for dev/unpackaged
// builds or an 'unknown' placeholder. A missing `sha` makes the Linux update
// decision treat the running build as "not the latest nightly" and offer the
// update, which is the safe default. Exported so main.ts can surface both in
// app:getInfo, and so the renderer's diagnostic contribute() snapshot can
// report exactly which commit of EACH repo is actually running — settling
// "is this build stale" from a single exported log instead of guesswork.
export function readBuildInfo(): BuildInfo {
    if (cachedBuildInfo) return cachedBuildInfo;
    try {
        const raw = fs.readFileSync(path.join(__dirname, 'build-info.json'), 'utf8');
        const info = JSON.parse(raw) as { sha?: string; coreSha?: string };
        cachedBuildInfo = {
            sha: info.sha && info.sha !== 'unknown' ? info.sha : null,
            coreSha: info.coreSha && info.coreSha !== 'unknown' ? info.coreSha : null,
        };
    } catch {
        cachedBuildInfo = { sha: null, coreSha: null };
    }
    return cachedBuildInfo;
}

function readBakedSha(): string | null {
    return readBuildInfo().sha;
}

// Download the nightly AppImage in the background and swap it in. Kicked off
// by checkNowLinux() AFTER it has already returned the 'downloading' status,
// so the renderer's "Check" button never blocks on the ~1.5GB fetch — it
// shows "update available" immediately and then live progress. Guards against
// overlapping downloads (a second check while one is running is a no-op here).
function startLinuxDownload(url: string, appImagePath: string, remoteSha: string, shortSha: string, myGeneration: number): void {
    if (linuxDownloadInFlight) return;
    diagLog('download start', { url, appImagePath, remoteSha });
    linuxDownloadInFlight = (async (): Promise<void> => {
        try {
            const dl = await fetch(url);
            if (!dl.ok || !dl.body) {
                throw new Error(`Download failed: HTTP ${dl.status}`);
            }
            // Stream to disk rather than buffering the whole ~1.5GB AppImage in
            // memory. Download next to the running AppImage so the rename below
            // stays on the same filesystem (required for it to be atomic).
            const tmpPath = `${appImagePath}.new`;
            const total = Number(dl.headers.get('content-length')) || 0;
            let received = 0;
            let lastPercent = -1;
            // Count bytes with a pass-through Transform in the pipeline rather
            // than a manual 'data' listener on the source: pipeline() then owns
            // the whole chain's completion + backpressure, so it reliably
            // resolves at end-of-stream (a stray 'data' listener on the source
            // can leave the download looking stuck at 100%).
            const counter = new Transform({
                transform(chunk: Buffer, _enc, cb) {
                    received += chunk.length;
                    if (total) {
                        const percent = Math.floor((received / total) * 100);
                        if (percent !== lastPercent) {
                            lastPercent = percent;
                            downloadPercent = percent;
                            broadcast(IPC_UPDATE_EVENT_PROGRESS, { percent, channel: currentChannel });
                            // Every 25% rather than every tick, to keep the
                            // diagnostic trace readable instead of flooded.
                            if (percent % 25 === 0) diagLog(`download progress ${percent}%`, { received, total });
                        }
                    }
                    cb(null, chunk);
                },
            });
            const body = Readable.fromWeb(dl.body as Parameters<typeof Readable.fromWeb>[0]);
            await pipeline(body, counter, fs.createWriteStream(tmpPath));
            fs.chmodSync(tmpPath, 0o755);
            fs.renameSync(tmpPath, appImagePath);
            linuxDownloadedSha = remoteSha;

            if (checkGeneration !== myGeneration) {
                diagLog('download finished but generation is stale — discarding', { myGeneration, checkGeneration });
                return;
            }
            pendingDownloaded = { version: shortSha };
            activeState = 'downloaded';
            downloadPercent = null;
            diagLog('download complete, swapped in', { received, appImagePath });
            broadcast(IPC_UPDATE_EVENT_DOWNLOADED, { version: shortSha, channel: currentChannel });
        } catch (err) {
            if (checkGeneration !== myGeneration) return;
            const message = err instanceof Error ? err.message : String(err);
            console.error('[update-manager] Linux download failed:', message);
            diagLog('download failed', { message });
            lastError = message;
            activeState = 'error';
            downloadPercent = null;
        } finally {
            linuxDownloadInFlight = null;
        }
    })();
}

async function checkNowLinux(): Promise<UpdateStatus> {
    const appImagePath = process.env.APPIMAGE;
    if (!appImagePath || currentChannel !== 'nightly') {
        diagLog('checkNow: unsupported', { appImagePath: appImagePath ?? null, currentChannel });
        return { status: 'unsupported', platform: 'linux' };
    }
    // A download already running means we've already found + reported the
    // update; just report the current (downloading) state without a redundant
    // metadata round-trip.
    if (linuxDownloadInFlight) {
        diagLog('checkNow: download already in flight, returning current status');
        return getStatus();
    }
    if (linuxInFlightCheck) {
        diagLog('checkNow: coalescing onto an already in-flight check');
        return linuxInFlightCheck;
    }
    const myGeneration = checkGeneration;
    activeState = 'checking';
    diagLog('checkNow: starting', { channel: currentChannel, appImagePath, generation: myGeneration });
    const run = (async (): Promise<UpdateStatus> => {
        try {
            const res = await fetch(GITHUB_NIGHTLY_RELEASE_API, { signal: AbortSignal.timeout(METADATA_FETCH_TIMEOUT_MS) });
            diagLog('checkNow: GitHub API responded', { status: res.status, url: GITHUB_NIGHTLY_RELEASE_API });
            if (!res.ok) {
                throw new Error(`GitHub API returned ${res.status}`);
            }
            const release = (await res.json()) as GithubRelease;
            if (checkGeneration !== myGeneration) {
                diagLog('checkNow: generation stale after fetch — discarding', { myGeneration, checkGeneration });
                return getStatus();
            }
            lastChecked = Date.now();
            lastError = null;

            const asset = release.assets.find((a) => a.name.endsWith('.AppImage'));
            if (!asset) {
                throw new Error('No .AppImage asset found in the nightly release');
            }
            const remoteSha = release.target_commitish;
            const shortSha = remoteSha.slice(0, 7);
            const bakedSha = readBakedSha();
            const decision = linuxUpdateDecision(bakedSha, remoteSha, linuxDownloadedSha);
            diagLog('checkNow: decision computed', { bakedSha, remoteSha, linuxDownloadedSha, decision, lastChecked });

            // Running build IS the latest nightly — nothing to do.
            if (decision === 'idle') {
                activeState = 'idle';
                pendingVersion = null;
                pendingDownloaded = null;
                downloadPercent = null;
                return getStatus();
            }
            // Already downloaded + staged this same nightly this session; keep
            // the pending-restart state rather than re-fetching the AppImage.
            if (decision === 'staged') {
                pendingVersion = shortSha;
                pendingDownloaded = { version: shortSha };
                activeState = 'downloaded';
                downloadPercent = null;
                return getStatus();
            }

            // decision === 'download': an update is available. Report it
            // immediately (status 'downloading', percent 0) and kick the actual
            // fetch off in the background — the renderer must not block on a
            // multi-minute ~1.5GB download.
            pendingVersion = shortSha;
            activeState = 'downloading';
            downloadPercent = 0;
            broadcast(IPC_UPDATE_EVENT_AVAILABLE, { version: shortSha, channel: currentChannel });
            broadcast(IPC_UPDATE_EVENT_PROGRESS, { percent: 0, channel: currentChannel });
            startLinuxDownload(asset.browser_download_url, appImagePath, remoteSha, shortSha, myGeneration);
            return getStatus();
        } catch (err) {
            if (checkGeneration !== myGeneration) return getStatus();
            const message = err instanceof Error ? err.message : String(err);
            console.error('[update-manager] Linux checkNow failed:', message);
            diagLog('checkNow: failed', { message });
            lastError = message;
            activeState = 'error';
        }
        return getStatus();
    })();
    linuxInFlightCheck = run;
    try {
        return await run;
    } finally {
        if (linuxInFlightCheck === run) {
            linuxInFlightCheck = null;
        }
    }
}

/**
 * Initialize the updater. Must be called once after `app.whenReady()` and
 * after at least one BrowserWindow exists (so the first broadcast lands).
 */
export function init(channel: UpdateChannel = 'stable'): void {
    currentChannel = channel;
    if (isLinux) {
        const buildInfo = readBuildInfo();
        diagLog('init', {
            platform: process.platform,
            appImagePath: process.env.APPIMAGE ?? null,
            channel,
            buildSha: buildInfo.sha,
            coreSha: buildInfo.coreSha,
        });
        initialCheckTimer = setTimeout(() => {
            initialCheckTimer = null;
            void checkNow();
        }, 30_000);
        pollTimer = setInterval(() => { void checkNow(); }, POLL_INTERVAL_MS);
        return;
    }
    try {
        createManager(channel);
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        console.error('[update-manager] Failed to construct Velopack UpdateManager:', message);
        lastError = message;
        activeState = 'error';
        return;
    }
    // Hydrate pending-restart state from the previous session. If the user
    // already downloaded an update and the app was restarted without applying
    // (or the apply failed), getUpdatePendingRestart() returns the pending
    // release from disk — no network call needed. Surfacing this immediately
    // ensures the restart banner renders without waiting for a fresh download.
    const alreadyPending = velopackUm!.getUpdatePendingRestart();
    if (alreadyPending) {
        // getUpdatePendingRestart() returns a VelopackAsset with a Version field
        // (not UpdateInfo.TargetFullRelease.Version — VelopackAsset is flat).
        const v = alreadyPending.Version;
        pendingVersion = v;
        pendingDownloaded = { version: v };
        activeState = 'downloaded';
        // Broadcast so any already-open windows show the banner immediately.
        broadcast(IPC_UPDATE_EVENT_DOWNLOADED, { version: v, channel: currentChannel });
    }
    // Kick the first check shortly after launch so we don't compete with the
    // splash/audio-engine bring-up for CPU + network. Store the handle so
    // shutdown() can cancel it if the user quits within the 30s window.
    initialCheckTimer = setTimeout(() => {
        initialCheckTimer = null;
        void checkNow();
    }, 30_000);
    pollTimer = setInterval(() => { void checkNow(); }, POLL_INTERVAL_MS);
}

/**
 * Switch release channel at runtime. Recreates the underlying Velopack
 * UpdateManager (the SDK has no in-place channel swap) and triggers an
 * immediate check so the renderer can update its banner without waiting for
 * the next 4h tick. On Linux this just re-evaluates the (channel-gated)
 * nightly checker.
 */
export function setChannel(channel: UpdateChannel): void {
    if (channel === currentChannel && (isLinux || velopackUm)) return;
    currentChannel = channel;
    pendingVersion = null;
    pendingDownloaded = null;
    downloadPercent = null;
    // Reset lastChecked too: until the immediate checkNow() below completes,
    // getStatus() should not report a stale "last checked" time that belongs
    // to the previous channel's feed.
    lastChecked = null;
    lastError = null;
    activeState = 'idle';
    // Bump the generation counter so any still-running check from the old
    // channel sees its epoch is stale and skips all state mutations +
    // broadcasts when its promise resolves. Also null the in-flight locks so
    // the new checkNow() below starts a fresh lock rather than coalescing
    // onto the old (stale) promise.
    checkGeneration++;
    inFlightCheck = null;
    linuxInFlightCheck = null;
    // The immediate checkNow() below supersedes init()'s pending boot check.
    // The renderer calls setChannel() on boot to sync the persisted channel,
    // so without this a non-stable channel would fire a second redundant
    // network check ~30s later when initialCheckTimer elapses.
    if (initialCheckTimer) {
        clearTimeout(initialCheckTimer);
        initialCheckTimer = null;
    }
    if (isLinux) {
        void checkNow();
        return;
    }
    try {
        createManager(channel);
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        console.error('[update-manager] Failed to switch channel:', message);
        lastError = message;
        activeState = 'error';
        return;
    }
    void checkNow();
}

/**
 * Trigger an immediate update check + download. Coalesces concurrent calls
 * (renderer button-mashing, overlapping poll timer) onto the same promise
 * so we don't fire parallel HTTP requests at the feed.
 */
export async function checkNow(): Promise<UpdateStatus> {
    if (isLinux) {
        return checkNowLinux();
    }
    if (!velopackUm) {
        return {
            status: 'error',
            channel: currentChannel,
            currentVersion: null,
            lastChecked,
            message: lastError ?? 'Update manager not initialized',
        };
    }
    if (inFlightCheck) {
        await inFlightCheck.catch(() => undefined);
        return getStatus();
    }
    // Capture the current generation before any await so we can detect a
    // concurrent setChannel() call that happened while this check was in flight.
    const myGeneration = checkGeneration;
    activeState = 'checking';
    const um = velopackUm;
    const thisCheck = um.checkForUpdatesAsync();
    inFlightCheck = thisCheck;
    try {
        const info = await thisCheck;
        // If the channel was switched while we were awaiting, discard all results
        // from this check — they belong to the old channel's feed.
        if (checkGeneration !== myGeneration) {
            return getStatus();
        }
        lastChecked = Date.now();
        lastError = null;
        if (!info) {
            // No release newer than what's installed. But an update may
            // already be downloaded and staged — by a prior session
            // (hydrated in init()) or an earlier check this session.
            // getUpdatePendingRestart() is the source of truth: a null
            // checkForUpdatesAsync() result does NOT mean "nothing pending"
            // (Velopack won't re-report an update already on disk), so
            // clearing pending state here would wrongly drop the restart
            // banner. Preserve it whenever a staged update still exists.
            const stillPending = um.getUpdatePendingRestart();
            if (stillPending) {
                pendingVersion = stillPending.Version;
                pendingDownloaded = { version: stillPending.Version };
                activeState = 'downloaded';
            } else {
                activeState = 'idle';
                pendingVersion = null;
                pendingDownloaded = null;
            }
            return getStatus();
        }
        const targetVersion = info.TargetFullRelease.Version;
        // Set pendingVersion immediately so getStatus() can surface the target
        // version in the 'downloading' state (before the download completes and
        // pendingDownloaded is set). Without this, the renderer shows version: ''
        // while the download is in progress.
        pendingVersion = targetVersion;
        activeState = 'downloading';
        broadcast(IPC_UPDATE_EVENT_AVAILABLE, { version: targetVersion, channel: currentChannel });
        await um.downloadUpdateAsync(info);
        // Re-check generation after the (potentially long) download.
        if (checkGeneration !== myGeneration) {
            return getStatus();
        }
        pendingDownloaded = { version: targetVersion };
        activeState = 'downloaded';
        broadcast(IPC_UPDATE_EVENT_DOWNLOADED, { version: targetVersion, channel: currentChannel });
    } catch (err) {
        if (checkGeneration !== myGeneration) {
            return getStatus();
        }
        const message = err instanceof Error ? err.message : String(err);
        console.error('[update-manager] checkNow failed:', message);
        lastError = message;
        activeState = 'error';
    } finally {
        // Only clear the lock if this invocation still owns it. If setChannel()
        // already nulled inFlightCheck and a new check started, we must not
        // clear that new check's lock.
        if (inFlightCheck === thisCheck) {
            inFlightCheck = null;
        }
    }
    return getStatus();
}

/**
 * Apply the downloaded update and restart the app.
 *
 * Windows/macOS: waitExitThenApplyUpdate() launches the Velopack updater and
 * tells it to wait for THIS process to exit — it does NOT exit us. We must
 * quit the app ourselves; the updater then swaps binaries and relaunches. It
 * only waits ~60s for our exit, so we quit promptly (on the next tick, so
 * this IPC call can return first). activeState is left at 'downloaded' so
 * that if the quit is vetoed or delayed the restart banner stays visible for
 * a retry.
 *
 * Linux: checkNowLinux() already replaced the AppImage file on disk (Linux
 * allows overwriting a file that's currently executing). There's nothing
 * left to "apply" — just launch the (already-new) AppImage as a detached
 * process and quit this one.
 */
export function applyAndRestart(): UpdateStatus {
    if (isLinux) {
        const appImagePath = process.env.APPIMAGE;
        diagLog('applyAndRestart: called', { appImagePath: appImagePath ?? null, activeState });
        if (!appImagePath) {
            return { status: 'unsupported', platform: 'linux' };
        }
        if (activeState !== 'downloaded' || !pendingDownloaded) {
            diagLog('applyAndRestart: nothing staged', { activeState, pendingDownloaded });
            return {
                status: 'error',
                channel: currentChannel,
                currentVersion: currentVersion(),
                lastChecked,
                message: 'No update is ready to apply',
            };
        }
        try {
            spawn(appImagePath, [], { detached: true, stdio: 'ignore' }).unref();
        } catch (err) {
            const message = err instanceof Error ? err.message : String(err);
            console.error('[update-manager] Linux relaunch failed:', message);
            diagLog('applyAndRestart: spawn failed', { message });
            lastError = message;
            activeState = 'error';
            return getStatus();
        }
        diagLog('applyAndRestart: spawned relaunch, quitting');
        setImmediate(() => app.quit());
        return getStatus();
    }
    if (!velopackUm) {
        return {
            status: 'error',
            channel: currentChannel,
            currentVersion: null,
            lastChecked,
            message: 'Update manager not initialized',
        };
    }
    const pending = velopackUm.getUpdatePendingRestart();
    if (!pending) {
        return {
            status: 'error',
            channel: currentChannel,
            currentVersion: currentVersion(),
            lastChecked,
            message: 'No update is ready to apply',
        };
    }
    try {
        // silent=false (show Velopack's restart UI on Windows), restart=true.
        velopackUm.waitExitThenApplyUpdate(pending, false, true);
    } catch (err) {
        const message = err instanceof Error ? err.message : String(err);
        console.error('[update-manager] applyAndRestart failed:', message);
        lastError = message;
        // A failed launch of the updater does not undo the download — the
        // staged package is still on disk. Keep activeState 'downloaded' so
        // the restart banner stays and the user can retry; only fall to
        // 'error' if the package somehow vanished.
        const stillPending = velopackUm.getUpdatePendingRestart();
        if (stillPending) {
            pendingVersion = stillPending.Version;
            pendingDownloaded = { version: stillPending.Version };
            activeState = 'downloaded';
        } else {
            activeState = 'error';
        }
        return {
            status: 'error',
            channel: currentChannel,
            currentVersion: currentVersion(),
            lastChecked,
            message,
        };
    }
    // The Velopack updater is now waiting for this process to exit (~60s
    // budget). Quit on the next tick so this IPC call returns to the renderer
    // first; activeState stays 'downloaded' until the process actually exits.
    setImmediate(() => app.quit());
    return getStatus();
}

export function getStatus(): UpdateStatus {
    if (isLinux && (!process.env.APPIMAGE || currentChannel !== 'nightly')) {
        return { status: 'unsupported', platform: 'linux' };
    }
    const base = {
        channel: currentChannel,
        currentVersion: currentVersion(),
        lastChecked,
    };
    switch (activeState) {
        case 'checking':
            return { status: 'checking', ...base };
        case 'downloading':
            return {
                status: 'downloading',
                ...base,
                // Use pendingVersion (set when download starts) so the renderer
                // can show the target version even before the download completes
                // and pendingDownloaded is populated.
                pending: { version: pendingVersion ?? '' },
                // Non-null only on Linux (Velopack doesn't report a percentage);
                // lets a panel that loads mid-download show progress immediately.
                percent: downloadPercent,
            };
        case 'downloaded':
            return {
                status: 'downloaded',
                ...base,
                pending: pendingDownloaded ?? { version: '' },
            };
        case 'error':
            return { status: 'error', ...base, message: lastError ?? 'Unknown error' };
        case 'idle':
        default:
            return { status: 'idle', ...base };
    }
}

/** Tear down the background poll and initial-check timers. Safe to call multiple times. */
export function shutdown(): void {
    if (initialCheckTimer) {
        clearTimeout(initialCheckTimer);
        initialCheckTimer = null;
    }
    if (pollTimer) {
        clearInterval(pollTimer);
        pollTimer = null;
    }
}
