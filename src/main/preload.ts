// Preload script — exposes safe APIs to the Slopsmith webview.
// The existing Slopsmith frontend runs unchanged; this adds
// window.slopsmithDesktop for audio engine and desktop features.

const { contextBridge, ipcRenderer } = require('electron');
import type { StartupStatus } from './python';
// Type-only imports (erased at compile) — no runtime require, so the preload
// bundle never drags in config-reset's electron/python/fs dependencies.
import type { ResetSelection, ResetSummary } from './config-reset';
import type { ConfigPathCategories } from './config-paths';

// Shape returned by maintenance.getPaths() — the enumerated per-OS categories
// plus the resolved active CONFIG_DIR and a flag for the shared Docker dir.
export interface MaintenancePaths {
    configDir: string;
    userData: string;
    dlcDir: string;
    pluginsDir: string;
    sharedDockerConfig: boolean;
    categories: ConfigPathCategories;
}
import {
    IPC_STARTUP_STATUS,
    IPC_STARTUP_GET_STATUS,
    IPC_STARTUP_REQUEST_STATUS,
    IPC_UPDATE_GET_STATUS,
    IPC_UPDATE_SET_CHANNEL,
    IPC_UPDATE_CHECK_NOW,
    IPC_UPDATE_APPLY,
    IPC_UPDATE_EVENT_AVAILABLE,
    IPC_UPDATE_EVENT_DOWNLOADED,
    IPC_POWER_SET_SCREEN_AWAKE,
    IPC_MAINTENANCE_GET_PATHS,
    IPC_MAINTENANCE_RESET,
    IPC_MAINTENANCE_RESTART,
} from './ipc-channels';

// Auto-update channel + event payloads. Kept here (rather than re-exported
// from update-manager.ts) so the preload bundle doesn't drag in the Velopack
// SDK — preload runs in a restricted context and we don't want native
// require()s evaluated here.
export type UpdateChannel = 'stable' | 'rc' | 'beta' | 'alpha';
export interface UpdateAvailablePayload { version: string; channel: UpdateChannel }
export interface UpdateDownloadedPayload { version: string; channel: UpdateChannel }

// Audio setDevice payload — duplex when input/output types match and device
// names match (or both empty); split otherwise. NodeAddon validates the
// shape and coerces sampleRate/bufferSize via Number() before forwarding.
export interface DeviceConfig {
    inputType: string;
    inputDevice: string;
    outputType: string;
    outputDevice: string;
    sampleRate: number;
    bufferSize: number;
}

// setDevice result. `duplex` reports the resolved mode (the engine may pick
// duplex even when the renderer sent type-mismatched devices, e.g. when the
// user lands on the same driver via different UI dropdowns). On failure
// `ok` is false and `error` carries the message; the other fields may still
// be set from a partial open before rollback.
export interface DeviceConfigResult {
    ok: boolean;
    error?: string;
    duplex: boolean;
    sampleRate?: number;
    inputBlockSize?: number;
    outputBlockSize?: number;
}

// Audio sync offset — set as a mutable property via the isolated world bridge.
// The settings panel reads/writes localStorage and updates this at runtime.

// Polyphonic chord-scoring request/response contract. Mirrors the C++
// ChordScorer::Request / Result structs and the JS _ndScoreChord
// payload — kept in sync by hand since N-API doesn't generate
// bindings. Optional `mt` field on Note isn't read by the scorer
// (matches JS) but is allowed in the request shape so callers can
// pass through the same chart-note objects they consume in JS.
export interface ChordScoreNote {
    s: number;   // 0-based string index
    f: number;   // fret
    mt?: number; // mute flag (ignored by scorer)
    ho?: boolean; // hammer-on
    po?: boolean; // pull-off
    b?: boolean;  // bend
    sl?: boolean; // slide
    hm?: boolean; // harmonic (energy-only check)
}
export interface ChordScoreRequest {
    // arrangement and stringCount are optional on the wire — the
    // native parser defaults them to 'guitar' / 6 when omitted, so a
    // request that supplies six standard-tuning offsets and notes
    // can leave them out entirely. They become effectively required
    // only when you want to score a non-default tuning (any bass
    // arrangement, or a 7/8-string guitar), since `offsets.length`
    // must equal `stringCount` for the scorer's validation to pass.
    arrangement?: 'guitar' | 'bass';
    stringCount?: number;
    offsets: number[];        // tuning offsets, length must equal stringCount (default 6)
    capo?: number;
    pitchCheckCents?: number; // 0 = energy-only chord check
    minHitRatio?: number;
    numSamples?: number;      // override the 4096 default window
    // Harmonic-comb verification controls (see NodeAddon ScoreChord docs).
    bypassMl?: boolean;        // force the DSP scorer even with an ML model loaded
    harmonicVerify?: boolean;  // score by harmonic-comb energy, not band/total
    harmonicSnr?: number;      // min harmonic-to-floor ratio for a hit (default 3.0)
    fundamentalRatio?: number; // fundamental-presence gate; lower for bass, <=0 disables (default 0.20)
    notes: ChordScoreNote[];
}
export interface ChordScoreNoteResult {
    s: number;
    f: number;
    hit: boolean;
    bandEnergy: number;
    centsDiff: number | null;
    centsError: number | null;
}
export interface ChordScoreResult {
    score: number;
    hitStrings: number;
    totalStrings: number;
    isHit: boolean;
    results: ChordScoreNoteResult[];
}

// One note in a chart pushed to the engine for continuous verification.
export interface ChartNote {
    id: string;        // stable note key (matches the plugin's noteKey)
    t: number;         // chart time of the onset, seconds
    s: number;         // string index
    f: number;         // fret
    sus: number;       // sustain length, seconds
    ho?: boolean;      // hammer-on
    po?: boolean;      // pull-off
    b?: boolean;       // bend
    sl?: boolean;      // slide
    hm?: boolean;      // harmonic
}
// The full song chart + scoring context, pushed once per arrangement load.
export interface ChartUpdate {
    arrangement?: 'guitar' | 'bass';
    stringCount?: number;
    tuningOffsets: number[];
    capo?: number;
    pitchCheckCents?: number;
    harmonicSnr?: number;
    fundamentalRatio?: number; // fundamental-presence gate; lower for bass, <=0 disables (default 0.20)
    presenceRatio?: number;    // temporal-persistence floor (0,1]; bass ~0.3, 0 = legacy ever-present
    timingTolerance?: number;  // seconds — half-width of the scoring window
    notes: ChartNote[];
}
// A finalized per-note verdict drained from the engine's NoteVerifier.
export interface NoteVerdict {
    id: string;
    detected: boolean;
    detectedSongTime: number;
    centsError: number;
    snr: number;
}

// Raw polyphonic transcription from the ML note detector (Basic Pitch).
export interface DetectedNote {
    midi: number;       // MIDI pitch, 21..108
    confidence: number; // note posteriorgram, 0..1
    onsetMs: number;    // ms since this pitch's onset (large = sustained tail)
    onsetSeq: number;   // per-pitch onset counter; a change == a new note struck
}
export interface NoteDetection {
    notes: DetectedNote[];
    sampleRate: number;
}

// Expose the privileged desktop bridge ONLY in the main frame. The window
// ships with webSecurity:false and this preload runs in every frame, so a
// remote sub-frame (e.g. the tutorials plugin's YouTube embed) would
// otherwise inherit the full IPC surface — audio engine, auto-update, etc.
// `window === window.top` is true only in the top frame; in a (cross-origin)
// sub-frame `window.top` is a different WindowProxy, so the identity check is
// false without throwing. main.ts's will-frame-navigate guard allow-lists the
// embed origins on the strength of this gate. Plugins are hydrated into the
// main document (not iframes) and window.open pop-outs are their own top-level
// frames, so neither loses the bridge.
const isMainFrame = (() => {
    // preload.ts compiles without the DOM lib, so reach the frame's window via
    // globalThis (identical to `window` in the renderer at runtime).
    const w = globalThis as unknown as { top?: unknown };
    try { return w === w.top; } catch { return false; }
})();

const slopsmithDesktopApi = {
    // Platform detection
    isDesktop: true,
    platform: process.platform,

    // Audio engine
    audio: {
        isAvailable: () => ipcRenderer.invoke('audio:isAvailable'),

        // Device management
        getDeviceTypes: () => ipcRenderer.invoke('audio:getDeviceTypes'),
        getSampleRates: () => ipcRenderer.invoke('audio:getSampleRates'),
        getBufferSizes: () => ipcRenderer.invoke('audio:getBufferSizes'),
        probeDeviceOptions: (
            inputTypeOrType: string,
            inputName: string,
            outputTypeOrOutputName: string,
            outputName?: string,
        ) => outputName === undefined
            ? ipcRenderer.invoke('audio:probeDeviceOptions', inputTypeOrType, inputName, outputTypeOrOutputName)
            : ipcRenderer.invoke('audio:probeDeviceOptions', inputTypeOrType, inputName, outputTypeOrOutputName, outputName),
        getCurrentDevice: () => ipcRenderer.invoke('audio:getCurrentDevice'),
        setDeviceType: (typeName: string) => ipcRenderer.invoke('audio:setDeviceType', typeName),
        setOutputDeviceType: (typeName: string) => ipcRenderer.invoke('audio:setOutputDeviceType', typeName),
        setDevice: ((
            arg0: DeviceConfig | string,
            arg1?: string,
            arg2?: number,
            arg3?: number,
        ): Promise<DeviceConfigResult> => arg1 === undefined
            ? ipcRenderer.invoke('audio:setDevice', arg0)
            : ipcRenderer.invoke('audio:setDevice', arg0, arg1, arg2, arg3)) as {
                (payload: DeviceConfig): Promise<DeviceConfigResult>;
                (input: string, output: string, sampleRate: number, bufferSize: number): Promise<DeviceConfigResult>;
            },
        getDeviceMetrics: () => ipcRenderer.invoke('audio:getDeviceMetrics'),
        loadDeviceSettings: () => ipcRenderer.invoke('audio:loadDeviceSettings'),
        saveDeviceSettings: (settings: unknown) => ipcRenderer.invoke('audio:saveDeviceSettings', settings),

        // Audio control
        startAudio: () => ipcRenderer.invoke('audio:startAudio'),
        stopAudio: () => ipcRenderer.invoke('audio:stopAudio'),
        isAudioRunning: () => ipcRenderer.invoke('audio:isAudioRunning'),

        // Gain
        setGain: (which: string, value: number) => ipcRenderer.invoke('audio:setGain', which, value),
        setInputChannel: (channel: number) => ipcRenderer.invoke('audio:setInputChannel', channel),
        setMonitorMute: (mute: boolean) => ipcRenderer.invoke('audio:setMonitorMute', mute),
        setMonitorMuteSuppressed: (suppressed: boolean) =>
            ipcRenderer.invoke('audio:setMonitorMuteSuppressed', suppressed),
        isMonitorMuted: () => ipcRenderer.invoke('audio:isMonitorMuted'),
        setNoiseGate: (payload: {
            enabled: boolean;
            thresholdDb: number;
            releaseMs: number;
            depthDb: number;
        }) => ipcRenderer.invoke('audio:setNoiseGate', payload),
        setTonePolish: (payload: { enabled: boolean }) =>
            ipcRenderer.invoke('audio:setTonePolish', payload),

        // Metering (polled at 60fps from renderer)
        getLevels: () => ipcRenderer.invoke('audio:getLevels'),
        getSourceLevels: (id: number) => ipcRenderer.invoke('audio:getSourceLevels', id),
        resetPeaks: () => ipcRenderer.invoke('audio:resetPeaks'),
        getBackingLevel: (): Promise<number> => ipcRenderer.invoke('audio:getBackingLevel'),

        // Pitch detection (polled). Backed by the polyphonic ML detector
        // (Basic Pitch) when a model is loaded, else the YIN detector —
        // same result shape either way.
        getPitchDetection: () => ipcRenderer.invoke('audio:getPitchDetection'),

        // Raw YIN pitch for tuners: always the continuous (sub-Hz) frequency and
        // real cents, bypassing the ML note-snapping. Reads the post-noise-gate
        // signal, so it goes silent (frequency -1) when the gate is closed.
        getRawPitch: () => ipcRenderer.invoke('audio:getRawPitch'),

        // Post-noise-gate raw mono audio frame (Float32Array of the most-recent
        // `numSamples` samples, default 4096, left-zero-padded on cold start) so
        // a tuner can run its own tuning-optimised pitch pipeline instead of the
        // shared YIN behind getRawPitch. Resolves an empty Float32Array on a
        // downlevel addon, so the caller feature-detects (length 0) and falls
        // back to getRawPitch.
        getRawAudioFrame: (numSamples?: number): Promise<Float32Array> =>
            ipcRenderer.invoke('audio:getRawAudioFrame', numSamples),

        // Whether the ML note detector (Basic Pitch) is active vs. the YIN
        // fallback. Resolves false on a downlevel addon.
        isMlNoteDetection: (): Promise<boolean> => ipcRenderer.invoke('audio:isMlNoteDetection'),

        // Current engine sample rate — needed by notedetect's chord
        // scorer to map FFT bins to Hz on the bridge path (no
        // AudioContext to read it from). Queried once at startAudio.
        getSampleRate: (): Promise<number> => ipcRenderer.invoke('audio:getSampleRate'),

        // Polyphonic chord scoring — native ChordScorer in the audio
        // engine evaluates the chord context against the most recent
        // input-ring samples and returns the same
        // `{ score, hitStrings, totalStrings, isHit, results[] }` shape
        // the in-renderer `_ndScoreChord` produces. No audio buffers
        // cross IPC; only the small result object does. Returns `null`
        // on a downlevel addon that predates ChordScorer so the caller
        // can fall back gracefully.
        scoreChord: (ctx: ChordScoreRequest): Promise<ChordScoreResult | null> =>
            ipcRenderer.invoke('audio:scoreChord', ctx),

        // Push the song's note chart into the engine for continuous,
        // background verification. The notedetect plugin calls this once per
        // arrangement load; the engine's NoteVerifier thread scores each note
        // against the live playhead, replacing the renderer's per-tick
        // scoreChord loop. Resolves null on a downlevel addon (pre-NoteVerifier)
        // so the caller feature-detects and keeps the old matchNotes path.
        setChart: (chart: ChartUpdate): Promise<boolean | null> =>
            ipcRenderer.invoke('audio:setChart', chart),

        // Drain the verdicts the engine's NoteVerifier has finalized since the
        // last call. Resolves null on a downlevel addon so the caller
        // feature-detects. The optional (songTime, playing) args push the
        // renderer's unified, already-corrected playhead — the verifier scores
        // against this rather than the JUCE backing transport (frozen for
        // HTML5-routed sloppak songs). Pass them every detect tick.
        getNoteVerdicts: (songTime?: number, playing?: boolean): Promise<NoteVerdict[] | null> =>
            ipcRenderer.invoke('audio:getNoteVerdicts', songTime, playing),

        // Raw polyphonic transcription — the ML note detector's full
        // active-pitch set. Resolves null when the ML detector isn't active
        // (downlevel addon, ONNX support absent, or no model loaded) so the
        // caller feature-detects and falls back to getPitchDetection /
        // scoreChord.
        detectNotes: (): Promise<NoteDetection | null> =>
            ipcRenderer.invoke('audio:detectNotes'),

        // ── Multi-input sources ──────────────────────────────────────────────
        // Each source is an independent input chain (own arrangement chart, note
        // detection, scoring, tone, monitor). sources[0] always exists and is
        // what the legacy un-suffixed methods above target. A renderer that wants
        // more than one player adds a source per extra input and drives its
        // scoring via the *Source* methods. All resolve to safe defaults on a
        // downlevel addon so the caller feature-detects.

        // Activate a pooled input chain bound to `inputChannel` (-1 = mono mix of
        // the first pair). Resolves the new sourceId, or -1 if the pool is full /
        // unsupported.
        // deviceKey routes the source to an additional bound input device (0 =
        // primary). Resolves the new sourceId, or -1 if the pool is full / unsupported.
        addSource: (inputChannel?: number, deviceKey?: number): Promise<number> =>
            ipcRenderer.invoke('audio:addSource', inputChannel, deviceKey),
        removeSource: (id: number): Promise<boolean> =>
            ipcRenderer.invoke('audio:removeSource', id),
        listSources: (): Promise<Array<{ id: number; inputChannel: number; deviceKey: number; active: boolean }> | null> =>
            ipcRenderer.invoke('audio:listSources'),
        // Phase 2 multi-device: list capture devices + bind/unbind an additional
        // input device (deviceKey 1..N) so a source can capture from it directly.
        listInputDevices: (): Promise<Array<{ typeName: string; name: string }> | null> =>
            ipcRenderer.invoke('audio:listInputDevices'),
        bindInputDevice: (deviceKey: number, deviceName: string): Promise<string> =>
            ipcRenderer.invoke('audio:bindInputDevice', deviceKey, deviceName),
        unbindInputDevice: (deviceKey: number): Promise<boolean> =>
            ipcRenderer.invoke('audio:unbindInputDevice', deviceKey),
        setSourceInputChannel: (id: number, channel: number): Promise<void> =>
            ipcRenderer.invoke('audio:setSourceInputChannel', id, channel),
        setSourceVerifierOffset: (id: number, seconds: number): Promise<void> =>
            ipcRenderer.invoke('audio:setSourceVerifierOffset', id, seconds),
        setSourceMonitorMute: (id: number, mute: boolean): Promise<void> =>
            ipcRenderer.invoke('audio:setSourceMonitorMute', id, mute),

        // Per-source twins of setChart / scoreChord / getNoteVerdicts /
        // getRawAudioFrame / getPitchDetection — same shapes, the leading id
        // selects the source.
        setSourceChart: (id: number, chart: ChartUpdate): Promise<boolean | null> =>
            ipcRenderer.invoke('audio:setSourceChart', id, chart),
        scoreSourceChord: (id: number, ctx: ChordScoreRequest): Promise<ChordScoreResult | null> =>
            ipcRenderer.invoke('audio:scoreSourceChord', id, ctx),
        getSourceNoteVerdicts: (id: number, songTime?: number, playing?: boolean): Promise<NoteVerdict[] | null> =>
            ipcRenderer.invoke('audio:getSourceNoteVerdicts', id, songTime, playing),
        getSourceRawAudioFrame: (id: number, numSamples?: number): Promise<Float32Array> =>
            ipcRenderer.invoke('audio:getSourceRawAudioFrame', id, numSamples),
        getSourcePitchDetection: (id: number) =>
            ipcRenderer.invoke('audio:getSourcePitchDetection', id),
        getSourceRawPitch: (id: number) =>
            ipcRenderer.invoke('audio:getSourceRawPitch', id),

        // VST plugins
        scanPlugins: (dirs?: string[]) => ipcRenderer.invoke('audio:scanPlugins', dirs),
        getKnownPlugins: () => ipcRenderer.invoke('audio:getKnownPlugins'),
        savePluginList: (path?: string) => ipcRenderer.invoke('audio:savePluginList', path),
        loadPluginList: (path?: string) => ipcRenderer.invoke('audio:loadPluginList', path),

        // Signal chain
        loadVST: (pluginPath: string) => ipcRenderer.invoke('audio:loadVST', pluginPath),
        loadNAMModel: (modelPath: string) => ipcRenderer.invoke('audio:loadNAMModel', modelPath),
        loadIR: (irPath: string) => ipcRenderer.invoke('audio:loadIR', irPath),
        removeProcessor: (slotId: number) => ipcRenderer.invoke('audio:removeProcessor', slotId),
        moveProcessor: (from: number, to: number) => ipcRenderer.invoke('audio:moveProcessor', from, to),
        setBypass: (slotId: number, bypassed: boolean) => ipcRenderer.invoke('audio:setBypass', slotId, bypassed),
        // Stereo routing (St-1/St-2): per-slot pan + parallel branch + L/R source.
        setPan: (slotId: number, pan: number) => ipcRenderer.invoke('audio:setPan', slotId, pan),
        setBranch: (slotId: number, branch: number) => ipcRenderer.invoke('audio:setBranch', slotId, branch),
        setBranchSrc: (slotId: number, src: number) => ipcRenderer.invoke('audio:setBranchSrc', slotId, src),
        clearChain: () => ipcRenderer.invoke('audio:clearChain'),
        getChainState: () => ipcRenderer.invoke('audio:getChainState'),

        // Plugin editor
        openPluginEditor: (slotId: number) => ipcRenderer.invoke('audio:openPluginEditor', slotId),
        closePluginEditor: (slotId: number) => ipcRenderer.invoke('audio:closePluginEditor', slotId),

        // Parameters
        getParameters: (slotId: number) => ipcRenderer.invoke('audio:getParameters', slotId),
        setParameter: (slotId: number, paramIndex: number, value: number) =>
            ipcRenderer.invoke('audio:setParameter', slotId, paramIndex, value),
        setSlotState: (slotId: number, base64State: string): Promise<boolean> =>
            ipcRenderer.invoke('audio:setSlotState', slotId, base64State),

        // MIDI
        sendMidiToSlot: (slotId: number, msgType: number, channel: number, param1: number, param2?: number) =>
            ipcRenderer.invoke('audio:sendMidiToSlot', slotId, msgType, channel, param1, param2),

        // Backing track
        loadBackingTrack: (filePath: string) => ipcRenderer.invoke('audio:loadBackingTrack', filePath),
        startBacking: () => ipcRenderer.invoke('audio:startBacking'),
        stopBacking: () => ipcRenderer.invoke('audio:stopBacking'),
        seekBacking: (seconds: number) => ipcRenderer.invoke('audio:seekBacking', seconds),
        getBackingPosition: (): Promise<number> => ipcRenderer.invoke('audio:getBackingPosition'),
        getBackingDuration: (): Promise<number> => ipcRenderer.invoke('audio:getBackingDuration'),
        isBackingPlaying: (): Promise<boolean> => ipcRenderer.invoke('audio:isBackingPlaying'),
        setBackingSpeed: (speed: number): Promise<boolean> => ipcRenderer.invoke('audio:setBackingSpeed', speed),

        // Presets
        savePreset: () => ipcRenderer.invoke('audio:savePreset'),
        loadPreset: (presetJson: string) => ipcRenderer.invoke('audio:loadPreset', presetJson),

        // Tone switching
        setMultiBypass: (changes: Array<{slotId: number, bypassed: boolean}>) =>
            ipcRenderer.invoke('audio:setMultiBypass', changes),
    },

    // Trusted executor for core audio-effects chain plans. Providers pass
    // opaque plan refs through core; desktop receives the private asset map,
    // validates the plan, and physically loads/applies native stages.
    audioEffects: {
        loadChainPlan: (request: unknown) => ipcRenderer.invoke('audio-effects:loadChainPlan', request),
        releaseRoute: (request: unknown) => ipcRenderer.invoke('audio-effects:releaseRoute', request),
        inspectRoute: (routeKey?: string) => ipcRenderer.invoke('audio-effects:inspectRoute', routeKey),
        activateSegment: (request: unknown) => ipcRenderer.invoke('audio-effects:activateSegment', request),
        setStageBypass: (request: unknown) => ipcRenderer.invoke('audio-effects:setStageBypass', request),
        setStageParameter: (request: unknown) => ipcRenderer.invoke('audio-effects:setStageParameter', request),
        setRouteGain: (request: unknown) => ipcRenderer.invoke('audio-effects:setRouteGain', request),
    },

    // Plugin management
    plugins: {
        install: (gitUrl: string, name?: string) => ipcRenderer.invoke('plugins:install', gitUrl, name),
        remove: (name: string) => ipcRenderer.invoke('plugins:remove', name),
        update: (name: string) => ipcRenderer.invoke('plugins:update', name),
        listInstalled: () => ipcRenderer.invoke('plugins:listInstalled'),
    },

    // Soundfont (Audio Quality preference for GP5 → audio rendering)
    soundfont: {
        getStatus: () => ipcRenderer.invoke('soundfont:getStatus'),
        downloadHighQuality: () => ipcRenderer.invoke('soundfont:downloadHighQuality'),
        cancelDownload: () => ipcRenderer.invoke('soundfont:cancelDownload'),
        setQuality: (quality: 'default' | 'high') => ipcRenderer.invoke('soundfont:setQuality', quality),
        onDownloadProgress: (
            callback: (progress: { bytesDownloaded: number; totalBytes: number; percent: number }) => void,
        ) => {
            const listener = (_event: unknown, progress: { bytesDownloaded: number; totalBytes: number; percent: number }) => callback(progress);
            ipcRenderer.on('soundfont:downloadProgress', listener);
            return () => ipcRenderer.removeListener('soundfont:downloadProgress', listener);
        },
    },

    // LAN access (opt-in: bind backend to 0.0.0.0 for multi-device / mobile sync)
    network: {
        getLanAccess: (): Promise<{ enabled: boolean; urls: string[] }> =>
            ipcRenderer.invoke('network:getLanAccess'),
        setLanAccess: (enabled: boolean): Promise<{ success: boolean; enabled: boolean; urls: string[] }> =>
            ipcRenderer.invoke('network:setLanAccess', enabled),
    },

    // Config maintenance — "Reset / repair configuration" (Settings panel).
    // getPaths returns the per-OS enumerated categories + the resolved CONFIG_DIR
    // (incl. a sharedDockerConfig flag); reset runs a granular delete; restart
    // relaunches the app once the user confirms.
    maintenance: {
        getPaths: (): Promise<MaintenancePaths> => ipcRenderer.invoke(IPC_MAINTENANCE_GET_PATHS),
        reset: (selection: ResetSelection): Promise<ResetSummary> =>
            ipcRenderer.invoke(IPC_MAINTENANCE_RESET, selection),
        restart: (): Promise<{ restarting: boolean }> => ipcRenderer.invoke(IPC_MAINTENANCE_RESTART),
    },

    // File dialogs
    pickFile: (filters?: { name: string; extensions: string[] }[]) =>
        ipcRenderer.invoke('dialog:pickFile', filters),
    pickDirectory: () => ipcRenderer.invoke('dialog:pickDirectory'),
    pickFiles: (filters?: { name: string; extensions: string[] }[]) =>
        ipcRenderer.invoke('dialog:pickFiles', filters),

    // App info
    getInfo: () => ipcRenderer.invoke('app:getInfo'),
    getConfigDir: () => ipcRenderer.invoke('app:getConfigDir'),

    // Startup status
    startup: {
        getStatus: () => ipcRenderer.invoke(IPC_STARTUP_GET_STATUS),
        onStatus: (callback: (status: StartupStatus) => void) => {
            const listener = (_event: unknown, status: StartupStatus) => callback(status);
            ipcRenderer.on(IPC_STARTUP_STATUS, listener);
            ipcRenderer.send(IPC_STARTUP_REQUEST_STATUS);
            return () => ipcRenderer.removeListener(IPC_STARTUP_STATUS, listener);
        },
    },

    // Auto-update (Velopack). The Settings panel reads/writes
    // localStorage['slopsmith-update-channel'] and mirrors it via setChannel.
    // Linux short-circuits to { status: "unsupported", platform: "linux" } on
    // every call — renderer should branch on that and surface a "download
    // from Releases" note rather than disabling the panel entirely.
    update: {
        getStatus: () => ipcRenderer.invoke(IPC_UPDATE_GET_STATUS),
        setChannel: (channel: UpdateChannel) => ipcRenderer.invoke(IPC_UPDATE_SET_CHANNEL, channel),
        checkNow: () => ipcRenderer.invoke(IPC_UPDATE_CHECK_NOW),
        apply: () => ipcRenderer.invoke(IPC_UPDATE_APPLY),
        onAvailable: (callback: (payload: UpdateAvailablePayload) => void) => {
            const listener = (_event: unknown, payload: UpdateAvailablePayload) => callback(payload);
            ipcRenderer.on(IPC_UPDATE_EVENT_AVAILABLE, listener);
            return () => ipcRenderer.removeListener(IPC_UPDATE_EVENT_AVAILABLE, listener);
        },
        onDownloaded: (callback: (payload: UpdateDownloadedPayload) => void) => {
            const listener = (_event: unknown, payload: UpdateDownloadedPayload) => callback(payload);
            ipcRenderer.on(IPC_UPDATE_EVENT_DOWNLOADED, listener);
            return () => ipcRenderer.removeListener(IPC_UPDATE_EVENT_DOWNLOADED, listener);
        },
    },
    // Keep the OS display/screensaver awake while a song plays. slopsmith core
    // app.js calls setScreenAwake(true) on play and (false) on pause/stop;
    // embedded Chromium ignores the renderer's navigator.wakeLock, so the main
    // process drives powerSaveBlocker instead (got-feedback/feedback#686).
    power: {
        setScreenAwake: (keep: boolean) => ipcRenderer.invoke(IPC_POWER_SET_SCREEN_AWAKE, keep),
    },
};

if (isMainFrame) {
    contextBridge.exposeInMainWorld('slopsmithDesktop', slopsmithDesktopApi);
}

export {};
