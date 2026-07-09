// Audio Bridge — connects the JUCE native addon to Electron IPC.
// The native addon is loaded via require() and its methods are
// exposed to the renderer process via ipcMain.handle().

import { ipcMain } from 'electron';
import * as path from 'path';
import * as fs from 'fs';
import { app } from 'electron';
import { isDebugEnabled, getDebugLogPath } from './debug-log';
import { initVstCrashGuard, armSentinel, disarmSentinel, armEditorSentinel, getSentinelPath } from './vst-crash-guard';
import { createAudioEffectsExecutor } from './audio-effects-executor';

type AudioModule = Record<string, (...args: any[]) => any>;

let audio: AudioModule | null = null;

type AudioDeviceSettings = {
    type: string;          // legacy alias = inputType when only type was stored
    inputType?: string;
    outputType?: string;
    input: string;
    output: string;
    sampleRate: string;
    bufferSize: string;
    inputChannel: string;
    monitorMute?: boolean;
    monitorKill?: boolean;
    savedAt?: number;
};

function asRecord(value: unknown): Record<string, unknown> | null {
    if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
    return value as Record<string, unknown>;
}

function normalizeStringArray(value: unknown): string[] {
    return Array.isArray(value) ? value.map(String) : [];
}

function normalizeNumberArray(value: unknown): number[] {
    if (!Array.isArray(value)) return [];
    return value.map(Number).filter((n) => Number.isFinite(n) && n > 0);
}

function isSelectValue(value: unknown): boolean {
    return typeof value === 'string' || (typeof value === 'number' && Number.isFinite(value));
}

function normalizeSelectValue(value: unknown): string {
    if (typeof value === 'string') return value;
    if (typeof value === 'number' && Number.isFinite(value)) return String(value);
    return '';
}

function normalizeDeviceSettings(settings: unknown): AudioDeviceSettings | null {
    const record = asRecord(settings);
    if (!record) return null;

    // Accept legacy ('type' only) or new ('inputType'+'outputType'). Legacy
    // values are mirrored into both slots so the engine stays in duplex.
    const hasLegacyType = typeof record.type === 'string';
    const hasDualType = typeof record.inputType === 'string' && typeof record.outputType === 'string';
    const hasExpectedShape =
        (hasLegacyType || hasDualType)
        && typeof record.input === 'string'
        && typeof record.output === 'string'
        && isSelectValue(record.sampleRate)
        && isSelectValue(record.bufferSize)
        && isSelectValue(record.inputChannel);
    if (!hasExpectedShape) return null;

    const legacyType = typeof record.type === 'string' ? record.type : '';
    const inputType = typeof record.inputType === 'string' ? record.inputType : legacyType;
    const outputType = typeof record.outputType === 'string' ? record.outputType : legacyType || inputType;

    const normalized: AudioDeviceSettings = {
        type: legacyType || inputType,
        inputType,
        outputType,
        input: typeof record.input === 'string' ? record.input : '',
        output: typeof record.output === 'string' ? record.output : '',
        sampleRate: normalizeSelectValue(record.sampleRate),
        bufferSize: normalizeSelectValue(record.bufferSize),
        inputChannel: normalizeSelectValue(record.inputChannel),
    };
    if (typeof record.monitorMute === 'boolean') {
        normalized.monitorMute = record.monitorMute;
    }
    if (typeof record.monitorKill === 'boolean') {
        normalized.monitorKill = record.monitorKill;
    }
    const savedAt = Number(record.savedAt);
    if (Number.isFinite(savedAt) && savedAt > 0) {
        normalized.savedAt = savedAt;
    }
    return normalized;
}

function normalizeDeviceOptions(
    options: unknown,
    fallback: {
        type: string;
        inputType?: string;
        outputType?: string;
        input: string;
        output: string;
        error?: string;
        compatible?: boolean;
    },
) {
    const record = asRecord(options);
    const inputType = String(record?.inputType ?? record?.type ?? fallback.inputType ?? fallback.type ?? '');
    const outputType = String(record?.outputType ?? record?.type ?? fallback.outputType ?? fallback.type ?? inputType);
    // Default to NOT compatible when the probe didn't say so — defaulting to
    // true here would leave Apply enabled in the addon-unavailable fallback
    // even though the same path also reports an error.
    const compatible = typeof record?.compatible === 'boolean'
        ? record.compatible
        : (fallback.compatible ?? false);
    return {
        type: String(record?.type ?? fallback.type ?? inputType),
        inputType,
        outputType,
        input: String(record?.input ?? fallback.input ?? ''),
        output: String(record?.output ?? fallback.output ?? ''),
        inputChannels: normalizeStringArray(record?.inputChannels),
        outputChannels: normalizeStringArray(record?.outputChannels),
        sampleRates: normalizeNumberArray(record?.sampleRates),
        bufferSizes: normalizeNumberArray(record?.bufferSizes),
        compatible,
        error: String(record?.error ?? fallback.error ?? ''),
    };
}

type DeviceConfigPayload = {
    inputType: string;
    inputDevice: string;
    outputType: string;
    outputDevice: string;
    sampleRate: number;
    bufferSize: number;
};

function normalizeDeviceConfigPayload(payload: unknown): DeviceConfigPayload {
    const record = asRecord(payload) || {};
    const inputType = String(record.inputType ?? record.type ?? '');
    const sampleRate = Number(record.sampleRate);
    const bufferSize = Number(record.bufferSize);
    return {
        inputType,
        inputDevice: String(record.inputDevice ?? record.input ?? ''),
        outputType: String(record.outputType ?? record.type ?? inputType),
        outputDevice: String(record.outputDevice ?? record.output ?? ''),
        sampleRate: Number.isFinite(sampleRate) && sampleRate > 0 ? sampleRate : 48000,
        bufferSize: Number.isFinite(bufferSize) && bufferSize > 0 ? bufferSize : 256,
    };
}

function normalizeDeviceConfigResult(result: unknown, fallbackError = '') {
    if (typeof result === 'boolean') {
        return {
            ok: result,
            error: result ? '' : fallbackError,
            duplex: true,
            sampleRate: 0,
            inputBlockSize: 0,
            outputBlockSize: 0,
        };
    }
    const record = asRecord(result);
    if (!record) {
        return {
            ok: false,
            error: fallbackError || 'Native audio device call returned an unsupported result',
            duplex: true,
            sampleRate: 0,
            inputBlockSize: 0,
            outputBlockSize: 0,
        };
    }
    const sampleRate = Number(record.sampleRate);
    const inputBlockSize = Number(record.inputBlockSize);
    const outputBlockSize = Number(record.outputBlockSize);
    return {
        ok: record.ok === true,
        error: String(record.error ?? fallbackError ?? ''),
        duplex: record.duplex !== false,
        sampleRate: Number.isFinite(sampleRate) ? sampleRate : 0,
        inputBlockSize: Number.isFinite(inputBlockSize) ? inputBlockSize : 0,
        outputBlockSize: Number.isFinite(outputBlockSize) ? outputBlockSize : 0,
    };
}

function getAudioSettingsPath(): string {
    return path.join(app.getPath('userData'), 'slopsmith-audio-settings.json');
}

function readAudioSettings(): AudioDeviceSettings | null {
    try {
        const settingsPath = getAudioSettingsPath();
        if (!fs.existsSync(settingsPath)) return null;
        return normalizeDeviceSettings(JSON.parse(fs.readFileSync(settingsPath, 'utf-8')));
    } catch (e: any) {
        console.warn(`[audio] Failed to read audio settings: ${e.message}`);
        return null;
    }
}

function writeAudioSettings(settings: unknown): boolean {
    const normalized = normalizeDeviceSettings(settings);
    if (!normalized) return false;
    try {
        const settingsPath = getAudioSettingsPath();
        fs.mkdirSync(path.dirname(settingsPath), { recursive: true });
        fs.writeFileSync(settingsPath, JSON.stringify(normalized, null, 2), 'utf-8');
        return true;
    } catch (e: any) {
        console.warn(`[audio] Failed to write audio settings: ${e.message}`);
        return false;
    }
}

// slotId → VST3 path, populated by audio:loadVST. Lets audio:openPluginEditor
// resolve a slot's plugin path for the crash sentinel without a native
// getChainState call. Kept in sync on remove/clear; slot ids are stable
// across moves so a reorder needs no upkeep.
const vstSlotPaths = new Map<number, string>();

function loadNativeAddon(): AudioModule | null {
    const addonPaths = [
        // Development build
        path.join(__dirname, '..', '..', 'build', 'Release', 'slopsmith_audio.node'),
        // Packaged build (unpacked from asar, the default for electron-builder asarUnpack entries)
        path.join(process.resourcesPath || '', 'app.asar.unpacked', 'build', 'Release', 'slopsmith_audio.node'),
        // Alternative location (direct copy to resources)
        path.join(process.resourcesPath || '', 'build', 'Release', 'slopsmith_audio.node'),
    ];

    for (const addonPath of addonPaths) {
        try {
            const mod = require(addonPath);
            console.log(`[audio] Loaded native addon from ${addonPath}`);
            return mod;
        } catch (e: any) {
            console.log(`[audio] Could not load from ${addonPath}: ${e.message}`);
        }
    }

    console.warn('[audio] Native audio addon not found — audio features disabled');
    console.warn('[audio] Build it with: npm run build:audio');
    return null;
}

export function initAudioBridge(): void {
    audio = loadNativeAddon();
    const audioEffects = createAudioEffectsExecutor(() => audio);

    if (audio) {
        // Redirect native stderr to the debug log before init() runs — that's
        // when the [AudioEngine] device diagnostics start. enableFileLogging
        // returns "" on success or an error description (with errno) so a
        // failure is diagnosable from the log itself. Best-effort.
        if (isDebugEnabled() && typeof audio.enableFileLogging === 'function') {
            try {
                const err = audio.enableFileLogging(getDebugLogPath());
                console.log(err
                    ? `[audio] Native file logging failed: ${err}`
                    : '[audio] Native file logging enabled');
            } catch (e: any) {
                console.warn(`[audio] enableFileLogging threw: ${e.message}`);
            }
        }

        try {
            audio.init();
            console.log('[audio] Engine initialized');
        } catch (e: any) {
            console.error(`[audio] Init failed: ${e.message}`);
            audio = null;
        }
    }

    // VST crash guard: promote any leftover crash sentinel into the blocklist,
    // then hand the blocklist to the addon so it sandboxes those plugins.
    if (audio) {
        try {
            const blocked = initVstCrashGuard();
            if (typeof audio.setCrashedPlugins === 'function')
                audio.setCrashedPlugins(blocked);
            if (blocked.length)
                console.log(`[audio] ${blocked.length} VST(s) on the crash blocklist — will load sandboxed`);
            // Arm the native last-chance attributor so a fatal in-process VST3
            // fault outside the load/editor sentinel windows (e.g. a plugin
            // WndProc on WM_ACTIVATEAPP) still stamps the sentinel and gets
            // sandboxed next launch. No-op on non-Windows. See issue #35.
            if (typeof audio.setVstCrashSentinelPath === 'function')
                audio.setVstCrashSentinelPath(getSentinelPath());
        } catch (e: any) {
            console.warn(`[audio] VST crash guard init failed: ${e.message}`);
        }
    }

    // Load the Basic Pitch ONNX model for the polyphonic ML note detector.
    // Bundled offline (Constitution IV) under resources/models/. Fail-soft:
    // a missing model / disabled ONNX support just leaves the engine on the
    // YIN PitchDetector / ChordScorer fallback (Constitution VII).
    if (audio && typeof audio.loadNoteModel === 'function') {
        const modelPath = app.isPackaged
            ? path.join(process.resourcesPath, 'models', 'basic_pitch.onnx')
            : path.join(__dirname, '..', '..', 'resources', 'models', 'basic_pitch.onnx');
        try {
            const ok = audio.loadNoteModel(modelPath);
            console.log(ok
                ? `[audio] ML note detection model loaded from ${modelPath}`
                : `[audio] ML note model unavailable (${modelPath}) — using YIN fallback`);
        } catch (e: any) {
            console.warn(`[audio] loadNoteModel failed: ${e.message} — using YIN fallback`);
        }
    }

    // Preset/plugin list paths
    const configDir = app.getPath('userData');

    // ── Lifecycle ──────────────────────────────────────────────────────────

    ipcMain.handle('audio:isAvailable', () => audio !== null);

    // ── Device Management ──────────────────────────────────────────────────

    ipcMain.handle('audio:getDeviceTypes', () => {
        const result = audio?.getDeviceTypes() ?? [];
        return result.map((t: any) => ({
            name: String(t.name || ''),
            inputs: Array.isArray(t.inputs) ? t.inputs.map(String) : [],
            outputs: Array.isArray(t.outputs) ? t.outputs.map(String) : [],
        }));
    });

    ipcMain.handle('audio:getSampleRates', () => {
        return audio?.getSampleRates() ?? [];
    });

    ipcMain.handle('audio:getBufferSizes', () => {
        return audio?.getBufferSizes() ?? [];
    });

    ipcMain.handle('audio:probeDeviceOptions', (_event, ...args: any[]) => {
        // (inputType, inputName, outputType, outputName) | legacy (type, input, output)
        const isDual = args.length >= 4;
        const inputType: string  = args[0] ?? '';
        const inputName: string  = args[1] ?? '';
        const outputType: string = isDual ? (args[2] ?? '') : inputType;
        const outputName: string = isDual ? (args[3] ?? '') : (args[2] ?? '');

        // Probe can throw from the JUCE backend (createDevice failures,
        // ASIO weirdness). Wrap so the IPC promise resolves with a
        // normalized incompatible payload instead of rejecting — the
        // renderer relies on a structured response to drive the warning
        // banner.
        let options: unknown = null;
        let probeError = '';
        if (audio && typeof audio.probeDeviceOptions === 'function') {
            try {
                options = isDual
                    ? audio.probeDeviceOptions(inputType, inputName, outputType, outputName)
                    : audio.probeDeviceOptions(inputType, inputName, outputName);
            } catch (e: unknown) {
                probeError = e instanceof Error ? e.message : String(e);
                console.warn(`[audio] probeDeviceOptions threw: ${probeError}`);
            }
            const normalized = normalizeDeviceOptions(options, {
                type: String(inputType || ''),
                inputType: String(inputType || ''),
                outputType: String(outputType || ''),
                input: String(inputName || ''),
                output: String(outputName || ''),
                error: probeError,
                compatible: false,
            });
            const looksLikeLegacyOverload = normalized.output === outputType;
            if (isDual && looksLikeLegacyOverload) {
                try {
                    const legacyOptions = audio.probeDeviceOptions(inputType, inputName, outputName);
                    return normalizeDeviceOptions(legacyOptions, {
                        type: String(inputType || ''),
                        inputType: String(inputType || ''),
                        outputType: String(outputType || inputType || ''),
                        input: String(inputName || ''),
                        output: String(outputName || ''),
                        error: '',
                        compatible: false,
                    });
                } catch (e: unknown) {
                    const legacyError = e instanceof Error ? e.message : String(e);
                    console.warn(`[audio] legacy probeDeviceOptions fallback threw: ${legacyError}`);
                    return { ...normalized, error: normalized.error || legacyError };
                }
            }
            return normalized;
        } else {
            probeError = 'Native audio addon not available';
        }
        return normalizeDeviceOptions(options, {
            type: String(inputType || ''),
            inputType: String(inputType || ''),
            outputType: String(outputType || ''),
            input: String(inputName || ''),
            output: String(outputName || ''),
            error: probeError,
            compatible: false,
        });
    });

    ipcMain.handle('audio:getCurrentDevice', () => {
        return audio?.getCurrentDevice() ?? null;
    });

    ipcMain.handle('audio:setDeviceType', (_event, typeName: string) => {
        return audio?.setDeviceType(typeName) ?? false;
    });

    ipcMain.handle('audio:setOutputDeviceType', (_event, typeName: string) => {
        if (!audio || typeof audio.setOutputDeviceType !== 'function') return false;
        try { return audio.setOutputDeviceType(typeName); }
        catch (e: any) {
            console.warn(`[audio] setOutputDeviceType failed: ${e?.message ?? e}`);
            return false;
        }
    });

    ipcMain.handle('audio:setDevice', (_event, ...args: any[]) => {
        if (!audio) return { ok: false, error: 'Native audio addon not available', duplex: true };
        // Object payload: setDevice({inputType, inputDevice, outputType, outputDevice, sampleRate, bufferSize})
        // Legacy positional: setDevice(input, output, sampleRate, bufferSize)
        // setAudioDeviceSetup can throw from the JUCE backend (ASIO drivers
        // are especially fond of this) — catch so the IPC promise resolves
        // with a structured error instead of rejecting into an unhandled
        // main-process exception.
        try {
            if (args.length === 1 && args[0] && typeof args[0] === 'object' && !Array.isArray(args[0])) {
                // NodeAddon's object-payload path expects numeric
                // sampleRate/bufferSize (it falls back to 48000/256 when
                // the key isn't a Number). Saved settings round-trip
                // through JSON / DOM strings, so coerce here so a
                // string-shaped "48000" doesn't silently reopen at the
                // fallback rate. Non-finite values fall through to the
                // native fallback (handled there too as defense-in-depth).
                const payload = { ...args[0] };
                const sr = Number(payload.sampleRate);
                const bs = Number(payload.bufferSize);
                if (Number.isFinite(sr) && sr > 0) payload.sampleRate = sr;
                if (Number.isFinite(bs) && bs > 0) payload.bufferSize = bs;
                const normalizedPayload = normalizeDeviceConfigPayload(payload);
                try {
                    return normalizeDeviceConfigResult(audio.setDevice(normalizedPayload));
                } catch (e: unknown) {
                    const objectError = e instanceof Error ? e.message : String(e);
                    console.warn(`[audio] setDevice object call threw: ${objectError}; retrying legacy device call`);
                    if (normalizedPayload.inputType !== normalizedPayload.outputType) {
                        return normalizeDeviceConfigResult(false, objectError);
                    }
                    try {
                        return normalizeDeviceConfigResult(
                            audio.setDevice(
                                normalizedPayload.inputDevice,
                                normalizedPayload.outputDevice,
                                normalizedPayload.sampleRate,
                                normalizedPayload.bufferSize,
                            ),
                            objectError,
                        );
                    } catch (legacyError: unknown) {
                        const error = legacyError instanceof Error ? legacyError.message : String(legacyError);
                        console.warn(`[audio] legacy setDevice fallback threw: ${error}`);
                        return { ok: false, error: error || objectError, duplex: true };
                    }
                }
            }
            const [input, output, sampleRate, bufferSize] = args;
            return normalizeDeviceConfigResult(audio.setDevice(input, output, sampleRate, bufferSize));
        } catch (e: unknown) {
            const error = e instanceof Error ? e.message : String(e);
            console.warn(`[audio] setDevice threw: ${error}`);
            return { ok: false, error, duplex: true };
        }
    });

    ipcMain.handle('audio:getDeviceMetrics', () => {
        if (!audio || typeof audio.getDeviceMetrics !== 'function') return null;
        try { return audio.getDeviceMetrics(); }
        catch (e: any) {
            console.warn(`[audio] getDeviceMetrics failed: ${e?.message ?? e}`);
            return null;
        }
    });

    ipcMain.handle('audio:loadDeviceSettings', () => readAudioSettings());

    ipcMain.handle('audio:saveDeviceSettings', (_event, settings: unknown) => writeAudioSettings(settings));

    // ── Audio Control ──────────────────────────────────────────────────────

    ipcMain.handle('audio:startAudio', () => {
        audio?.startAudio();
    });

    ipcMain.handle('audio:stopAudio', () => {
        audio?.stopAudio();
    });

    ipcMain.handle('audio:isAudioRunning', () => {
        return audio?.isAudioRunning() ?? false;
    });

    // ── Gain ───────────────────────────────────────────────────────────────

    ipcMain.handle('audio:setGain', (_event, which: string, value: number) => {
        audio?.setGain(which, value);
    });

    ipcMain.handle('audio:setInputChannel', (_event, channel: number) => {
        audio?.setInputChannel(channel);
    });

    ipcMain.handle('audio:setMonitorMute', (_event, mute: boolean) => {
        audio?.setMonitorMute(mute);
    });

    ipcMain.handle('audio:setMonitorMuteSuppressed', (_event, suppressed: boolean) => {
        // typeof-guarded: a downlevel addon without this method is a no-op
        // rather than a thrown IPC error (Constitution VII fail-soft).
        // Coerce to a real boolean so an unexpected non-boolean caller can't
        // make the N-API binding throw on As<Napi::Boolean>().
        if (audio && typeof audio.setMonitorMuteSuppressed === 'function') {
            audio.setMonitorMuteSuppressed(Boolean(suppressed));
        }
    });

    ipcMain.handle('audio:isMonitorMuted', () => {
        return audio?.isMonitorMuted() ?? true;
    });

    ipcMain.handle('audio:setMonitorKill', (_event, kill: boolean) => {
        // typeof-guarded fail-soft: a downlevel addon without setMonitorKill is a
        // no-op rather than a thrown IPC error. Coerced to a real boolean so the
        // N-API IsBoolean() guard sees a clean value.
        if (audio && typeof audio.setMonitorKill === 'function') {
            audio.setMonitorKill(Boolean(kill));
        }
    });

    ipcMain.handle(
        'audio:setNoiseGate',
        (
            _event,
            payload: {
                enabled: boolean;
                thresholdDb: number;
                releaseMs: number;
                depthDb: number;
            },
        ) => {
            audio?.setNoiseGate(payload);
        },
    );

    ipcMain.handle(
        'audio:setTonePolish',
        (_event, payload: { enabled: boolean }) => {
            if (audio && typeof audio.setTonePolish === 'function') {
                audio.setTonePolish(payload);
            }
        },
    );

    // ── Metering ───────────────────────────────────────────────────────────

    ipcMain.handle('audio:getLevels', () => {
        return audio?.getLevels() ?? { inputLevel: 0, outputLevel: 0, inputPeak: 0, outputPeak: 0 };
    });

    // Per-source input level (for a bound detector's silence gate on an extra device).
    ipcMain.handle('audio:getSourceLevels', (_event, id: unknown) => {
        const zero = { inputLevel: 0, outputLevel: 0, inputPeak: 0, outputPeak: 0 };
        if (!audio || typeof audio.getSourceLevels !== 'function' || !validSourceId(id)) return zero;
        try {
            return audio.getSourceLevels(id);
        } catch (e: unknown) {
            console.warn(`[audio] getSourceLevels failed: ${e instanceof Error ? e.message : String(e)}`);
            return zero;
        }
    });

    ipcMain.handle('audio:resetPeaks', () => {
        audio?.resetPeaks();
    });

    // Backing-track mix bus RMS level — reads the engine's per-block running RMS
    // after the backing volume fader. typeof-guarded so a downlevel addon that
    // predates getBackingLevel returns 0 instead of throwing (fail-soft per
    // Constitution VII).
    ipcMain.handle('audio:getBackingLevel', () => {
        if (!audio || typeof audio.getBackingLevel !== 'function')
            return 0;
        return audio.getBackingLevel();
    });

    // ── Pitch Detection ────────────────────────────────────────────────────

    ipcMain.handle('audio:getPitchDetection', () => {
        return audio?.getPitchDetection() ?? { frequency: -1, confidence: 0, midiNote: -1, cents: 0, noteName: '' };
    });

    // Raw YIN pitch, always — bypasses the ML preference so the tuner gets a
    // continuous (sub-Hz) frequency and real cents even when a Basic Pitch model
    // is loaded. The YIN detector reads the post-noise-gate signal, so this is
    // silent when the gate is closed. typeof-guarded so a downlevel addon (no
    // getRawPitchDetection) fails soft to the empty detection instead of throwing.
    ipcMain.handle('audio:getRawPitch', () => {
        if (!audio || typeof audio.getRawPitchDetection !== 'function')
            return { frequency: -1, confidence: 0, midiNote: -1, cents: 0, noteName: '' };
        try {
            return audio.getRawPitchDetection();
        } catch {
            return { frequency: -1, confidence: 0, midiNote: -1, cents: 0, noteName: '' };
        }
    });

    // Post-noise-gate raw mono audio frame (Float32Array of the most-recent N
    // samples) so the tuner can run its own tuning-optimised pitch pipeline
    // instead of the shared YIN detector behind getRawPitch. numSamples is
    // optional (defaults to 4096; the engine clamps to its ring capacity).
    // typeof-guarded + try/catch so a downlevel addon (no getRawAudioFrame)
    // fails soft to an empty array instead of throwing.
    ipcMain.handle('audio:getRawAudioFrame', (_event, numSamples?: number) => {
        if (!audio || typeof audio.getRawAudioFrame !== 'function')
            return new Float32Array(0);
        try {
            const n = Number(numSamples);
            return audio.getRawAudioFrame(Number.isFinite(n) && n > 0 ? n : 4096);
        } catch {
            return new Float32Array(0);
        }
    });

    // Whether the polyphonic ML note detector (Basic Pitch) is active. When
    // false the engine is on the YIN PitchDetector / ChordScorer fallback.
    // typeof-guarded so a downlevel addon simply reports false.
    ipcMain.handle('audio:isMlNoteDetection', () => {
        if (!audio || typeof audio.isMlNoteDetection !== 'function') return false;
        try {
            return audio.isMlNoteDetection() === true;
        } catch {
            return false;
        }
    });

    // Arm/suspend the ML note-detection pipeline. note_detect calls this true
    // only while a consumer actually reads ML notes (native-frame detection /
    // non-verifier fallback) and false otherwise, so the default harmonic-comb
    // verifier path and the always-on home tuner cost no ONNX inference.
    // typeof-guarded so a downlevel addon (no gate) simply ignores it — ML then
    // runs as before, i.e. fail-safe to current behaviour.
    ipcMain.handle('audio:setNoteDetectionEnabled', (_event, enabled: boolean) => {
        if (!audio || typeof audio.setNoteDetectionEnabled !== 'function') return;
        try {
            audio.setNoteDetectionEnabled(Boolean(enabled));
        } catch (e) {
            console.warn(`[audio] setNoteDetectionEnabled failed: ${e instanceof Error ? e.message : String(e)}`);
        }
    });

    // ── Chord Scoring (polyphonic) ─────────────────────────────────────────
    // The notedetect plugin's chord-scoring branch hands us a chord
    // context — notes, arrangement, tuning offsets, thresholds — and
    // we score it against the engine's most recent input-ring samples
    // entirely inside the native scorer. No audio buffers cross the
    // N-API boundary; only the small result object travels over IPC.

    ipcMain.handle('audio:getSampleRate', () => {
        // typeof guard covers the version-skew case where the JS/TS was
        // updated but the native addon predates getSampleRate — without
        // it, `audio.getSampleRate()` would throw rather than fall back.
        // The native side already substitutes 48000 for non-finite / <=0
        // values, but we double-check here so a malformed return value
        // from a stub or test harness can't leak through to the renderer
        // and feed a zero into downstream tolerance math.
        if (audio && typeof audio.getSampleRate === 'function') {
            try {
                const sr = audio.getSampleRate();
                if (typeof sr === 'number' && Number.isFinite(sr) && sr > 0) return sr;
            } catch { /* fall through */ }
        }
        return 48000;
    });

    // Why this is a synchronous handler (and not an N-API AsyncWorker /
    // worker thread): the only caller is the notedetect plugin's
    // `processFrame()` tick, which fires at ~20 Hz and awaits each
    // result before issuing the next — natural back-pressure, no
    // queueing or coalescing needed. A 16384-point juce::dsp::FFT at
    // 48 kHz takes ~0.5 ms on modern x86, plus negligible per-string
    // band-energy work, for a per-call cost well under the 50 ms
    // budget. The JS path it replaces runs the same FFT synchronously
    // in the renderer event loop today; moving to async would also
    // require a mutex around ChordScorer's reusable FFT/scratch state.
    // The trade-off doesn't pay back for this workload — revisit only
    // if profiling shows actual main-loop stalls.
    ipcMain.handle('audio:scoreChord', (_event, ctx: unknown) => {
        // Feature-detect the native method the same way getSampleRate
        // above does — a downlevel addon (pre-ChordScorer build) should
        // return null so the renderer can fall back to skipping chord
        // scoring instead of throwing on every call.
        if (!audio || typeof audio.scoreChord !== 'function') return null;
        try {
            return audio.scoreChord(ctx);
        } catch (e: unknown) {
            console.warn(`[audio] scoreChord failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    // Push the song's note chart into the engine for continuous, background
    // verification. The notedetect plugin calls this once per arrangement
    // load; the engine's NoteVerifier thread then scores each note against
    // the live playhead, replacing the renderer's per-tick scoreChord loop.
    // Returns null on a downlevel addon (pre-NoteVerifier) so the renderer
    // feature-detects and keeps the old matchNotes path.
    ipcMain.handle('audio:setChart', (_event, chart: unknown) => {
        if (!audio || typeof audio.setChart !== 'function') return null;
        try {
            return audio.setChart(chart);
        } catch (e: unknown) {
            console.warn(`[audio] setChart failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    // Drain the verdicts the NoteVerifier thread has finalized since the last
    // call. Returns null on a downlevel addon so the renderer feature-detects.
    // The optional (songTime, playing) args push the renderer's unified
    // playhead — the plugin calls this once per detect tick, so the push rides
    // the same IPC as the drain.
    ipcMain.handle('audio:getNoteVerdicts', (_event, songTime: unknown, playing: unknown) => {
        if (!audio || typeof audio.getNoteVerdicts !== 'function') return null;
        try {
            if (typeof songTime === 'number' && Number.isFinite(songTime)
                && typeof playing === 'boolean') {
                return audio.getNoteVerdicts(songTime, playing);
            }
            return audio.getNoteVerdicts();
        } catch (e: unknown) {
            console.warn(`[audio] getNoteVerdicts failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    // ── Multi-input sources ────────────────────────────────────────────────
    // Each source is an independent input chain (own arrangement chart, note
    // detection, scoring, tone, monitor). sources[0] always exists; the legacy
    // un-suffixed methods above target it. All of these feature-detect so a
    // downlevel addon (pre-multi-source) is a clean no-op, not a thrown IPC error.

    // A sourceId must be a non-negative integer. typeof===number alone is unsafe:
    // NaN/Infinity/0.5 pass it, and the native Int32Value() coerces NaN/Inf to 0,
    // which would silently retarget the LEGACY source 0 (the main player) — e.g.
    // setSourceChart(NaN, ...) overwriting source 0's chart. Reject non-integers.
    const validSourceId = (id: unknown): id is number =>
        typeof id === 'number' && Number.isInteger(id) && id >= 0;
    const validChannel = (ch: unknown): ch is number =>
        typeof ch === 'number' && Number.isInteger(ch);
    // deviceKey 0 = primary device; 1..N = an additional bound input device.
    const validDeviceKey = (k: unknown): k is number =>
        typeof k === 'number' && Number.isInteger(k) && k >= 0;

    // addSource(inputChannel?, deviceKey?) -> sourceId (number), or -1 if the pool
    // is full / unsupported. -1 lets the renderer detect "no more sources" the same
    // as a missing method. deviceKey routes the source to an additional device.
    ipcMain.handle('audio:addSource', (_event, inputChannel: unknown, deviceKey: unknown) => {
        if (!audio || typeof audio.addSource !== 'function') return -1;
        try {
            return audio.addSource(
                validChannel(inputChannel) ? inputChannel : -1,
                validDeviceKey(deviceKey) ? deviceKey : 0,
            );
        } catch (e: unknown) {
            console.warn(`[audio] addSource failed: ${e instanceof Error ? e.message : String(e)}`);
            return -1;
        }
    });

    // listInputDevices() -> [{ typeName, name }] | null. Capture devices available
    // to bind as additional engine inputs.
    ipcMain.handle('audio:listInputDevices', () => {
        if (!audio || typeof audio.listInputDevices !== 'function') return null;
        try {
            return audio.listInputDevices();
        } catch (e: unknown) {
            console.warn(`[audio] listInputDevices failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    // bindInputDevice(deviceKey, deviceName) -> "" on success or an error string
    // (or a non-empty sentinel when unsupported).
    ipcMain.handle('audio:bindInputDevice', (_event, deviceKey: unknown, deviceName: unknown) => {
        if (!audio || typeof audio.bindInputDevice !== 'function') return 'unsupported';
        if (!validDeviceKey(deviceKey) || deviceKey < 1) return 'invalid deviceKey';
        if (typeof deviceName !== 'string' || deviceName.length === 0) return 'invalid deviceName';
        try {
            return audio.bindInputDevice(deviceKey, deviceName);
        } catch (e: unknown) {
            const msg = e instanceof Error ? e.message : String(e);
            console.warn(`[audio] bindInputDevice failed: ${msg}`);
            return msg;
        }
    });

    ipcMain.handle('audio:unbindInputDevice', (_event, deviceKey: unknown) => {
        if (!audio || typeof audio.unbindInputDevice !== 'function') return false;
        if (!validDeviceKey(deviceKey) || deviceKey < 1) return false;
        try {
            return audio.unbindInputDevice(deviceKey);
        } catch (e: unknown) {
            console.warn(`[audio] unbindInputDevice failed: ${e instanceof Error ? e.message : String(e)}`);
            return false;
        }
    });

    // ── Streamer mix output (PR1) ───────────────────────────────────────────
    ipcMain.handle('audio:setStreamOutputDevice', (_event, typeName: unknown, deviceName: unknown) => {
        if (!audio || typeof audio.setStreamOutputDevice !== 'function') return 'unsupported';
        if (typeof typeName !== 'string' || typeof deviceName !== 'string') return 'invalid arguments';
        try {
            return audio.setStreamOutputDevice(typeName, deviceName);
        } catch (e: unknown) {
            const msg = e instanceof Error ? e.message : String(e);
            console.warn(`[audio] setStreamOutputDevice failed: ${msg}`);
            return msg;
        }
    });

    ipcMain.handle('audio:clearStreamOutput', () => {
        if (audio && typeof audio.clearStreamOutput === 'function') audio.clearStreamOutput();
    });

    ipcMain.handle('audio:setStreamBus', (_event, includeBacking: unknown, includeGuitar: unknown, gain: unknown) => {
        if (audio && typeof audio.setStreamBus === 'function') {
            // Require real booleans (don't Boolean()-coerce — that turns the string
            // "false" into true) and a finite gain (native also clamps/finite-guards).
            const ib = typeof includeBacking === 'boolean' ? includeBacking : true;
            const ig = typeof includeGuitar === 'boolean' ? includeGuitar : true;
            const g = typeof gain === 'number' && Number.isFinite(gain) ? gain : 1.0;
            audio.setStreamBus(ib, ig, g);
        }
    });

    ipcMain.handle('audio:setStreamBusGain', (_event, gain: unknown) => {
        if (audio && typeof audio.setStreamBusGain === 'function'
            && typeof gain === 'number' && Number.isFinite(gain)) {
            audio.setStreamBusGain(gain);
        }
    });

    // ── Renderer-audio bus (Phase 2: WebAudio master → engine output) ────────
    ipcMain.handle('audio:setRendererBus', (_event, enabled: unknown, gain: unknown) => {
        if (audio && typeof audio.setRendererBus === 'function') {
            const en = enabled === true;
            const g = typeof gain === 'number' && Number.isFinite(gain) ? gain : 1.0;
            audio.setRendererBus(en, g);
        }
    });

    // Push path uses `on` (fire-and-forget), not `handle`: chunks arrive ~50-100×
    // per second and an invoke round-trip per chunk doubles the IPC cost for a
    // reply nobody reads. Health is observed via getRendererBusMetrics instead.
    ipcMain.on('audio:pushRendererAudio', (_event, chunk: unknown, sourceRate: unknown) => {
        if (audio && typeof audio.pushRendererAudio === 'function'
            && chunk instanceof Float32Array && chunk.length >= 2) {
            const rate = typeof sourceRate === 'number' && Number.isFinite(sourceRate) ? sourceRate : 0;
            audio.pushRendererAudio(chunk, rate);
        }
    });

    ipcMain.handle('audio:getRendererBusMetrics', () => {
        if (!audio || typeof audio.getRendererBusMetrics !== 'function') return null;
        return audio.getRendererBusMetrics();
    });

    ipcMain.handle('audio:getStreamSinkLevel', () => {
        if (!audio || typeof audio.getStreamSinkLevel !== 'function') return 0;
        return audio.getStreamSinkLevel();
    });

    ipcMain.handle('audio:isStreamOutputActive', () => {
        if (!audio || typeof audio.isStreamOutputActive !== 'function') return false;
        return audio.isStreamOutputActive();
    });

    ipcMain.handle('audio:getStreamUnderflowCount', () => {
        if (!audio || typeof audio.getStreamUnderflowCount !== 'function') return 0;
        return audio.getStreamUnderflowCount();
    });

    ipcMain.handle('audio:getStreamOverflowCount', () => {
        if (!audio || typeof audio.getStreamOverflowCount !== 'function') return 0;
        return audio.getStreamOverflowCount();
    });

    ipcMain.handle('audio:removeSource', (_event, id: unknown) => {
        if (!audio || typeof audio.removeSource !== 'function') return false;
        if (!validSourceId(id)) return false;
        try {
            return audio.removeSource(id);
        } catch (e: unknown) {
            console.warn(`[audio] removeSource failed: ${e instanceof Error ? e.message : String(e)}`);
            return false;
        }
    });

    ipcMain.handle('audio:listSources', () => {
        if (!audio || typeof audio.listSources !== 'function') return null;
        try {
            return audio.listSources();
        } catch (e: unknown) {
            console.warn(`[audio] listSources failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    ipcMain.handle('audio:setSourceInputChannel', (_event, id: unknown, channel: unknown) => {
        if (!audio || typeof audio.setSourceInputChannel !== 'function') return;
        if (!validSourceId(id) || !validChannel(channel)) return;
        try {
            audio.setSourceInputChannel(id, channel);
        } catch (e: unknown) {
            console.warn(`[audio] setSourceInputChannel failed: ${e instanceof Error ? e.message : String(e)}`);
        }
    });

    ipcMain.handle('audio:setSourceVerifierOffset', (_event, id: unknown, seconds: unknown) => {
        if (!audio || typeof audio.setSourceVerifierOffset !== 'function') return;
        if (!validSourceId(id) || typeof seconds !== 'number' || !Number.isFinite(seconds)) return;
        try {
            audio.setSourceVerifierOffset(id, seconds);
        } catch (e: unknown) {
            console.warn(`[audio] setSourceVerifierOffset failed: ${e instanceof Error ? e.message : String(e)}`);
        }
    });

    ipcMain.handle('audio:setSourceMonitorMute', (_event, id: unknown, mute: unknown) => {
        if (!audio || typeof audio.setSourceMonitorMute !== 'function') return;
        // Require a real boolean — don't truthiness-coerce a bad arg (e.g. the
        // string "false") into a real mute.
        if (!validSourceId(id) || typeof mute !== 'boolean') return;
        try {
            audio.setSourceMonitorMute(id, mute);
        } catch (e: unknown) {
            console.warn(`[audio] setSourceMonitorMute failed: ${e instanceof Error ? e.message : String(e)}`);
        }
    });

    // Per-source twins of setChart / scoreChord / getNoteVerdicts / getRawAudio-
    // Frame / getPitchDetection. Same shapes as the legacy methods; the leading
    // id selects the source. Null / -1 / safe defaults on a downlevel addon or
    // an invalid id (so a bad id can never fall through to source 0).
    ipcMain.handle('audio:setSourceChart', (_event, id: unknown, chart: unknown) => {
        if (!audio || typeof audio.setSourceChart !== 'function') return null;
        if (!validSourceId(id)) return false;
        try {
            return audio.setSourceChart(id, chart);
        } catch (e: unknown) {
            console.warn(`[audio] setSourceChart failed: ${e instanceof Error ? e.message : String(e)}`);
            return false;
        }
    });

    ipcMain.handle('audio:scoreSourceChord', (_event, id: unknown, ctx: unknown) => {
        if (!audio || typeof audio.scoreSourceChord !== 'function') return null;
        if (!validSourceId(id)) return null;
        try {
            return audio.scoreSourceChord(id, ctx);
        } catch (e: unknown) {
            console.warn(`[audio] scoreSourceChord failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    ipcMain.handle('audio:getSourceNoteVerdicts', (_event, id: unknown, songTime: unknown, playing: unknown) => {
        if (!audio || typeof audio.getSourceNoteVerdicts !== 'function') return null;
        if (!validSourceId(id)) return null;
        try {
            if (typeof songTime === 'number' && Number.isFinite(songTime)
                && typeof playing === 'boolean') {
                return audio.getSourceNoteVerdicts(id, songTime, playing);
            }
            return audio.getSourceNoteVerdicts(id);
        } catch (e: unknown) {
            console.warn(`[audio] getSourceNoteVerdicts failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    ipcMain.handle('audio:getSourceRawAudioFrame', (_event, id: unknown, numSamples?: unknown) => {
        if (!audio || typeof audio.getSourceRawAudioFrame !== 'function'
            || !validSourceId(id)) {
            return new Float32Array(0);
        }
        try {
            const n = typeof numSamples === 'number' ? numSamples : 4096;
            return audio.getSourceRawAudioFrame(id, Number.isFinite(n) && n > 0 ? n : 4096);
        } catch (e: unknown) {
            console.warn(`[audio] getSourceRawAudioFrame failed: ${e instanceof Error ? e.message : String(e)}`);
            return new Float32Array(0);
        }
    });

    ipcMain.handle('audio:getSourcePitchDetection', (_event, id: unknown) => {
        if (!audio || typeof audio.getSourcePitchDetection !== 'function'
            || !validSourceId(id)) {
            return { frequency: -1, confidence: 0, midiNote: -1, cents: 0, noteName: '' };
        }
        try {
            return audio.getSourcePitchDetection(id);
        } catch (e: unknown) {
            console.warn(`[audio] getSourcePitchDetection failed: ${e instanceof Error ? e.message : String(e)}`);
            return { frequency: -1, confidence: 0, midiNote: -1, cents: 0, noteName: '' };
        }
    });

    // Per-source raw YIN detection (bypasses ML) — backs the sustain glow / mono
    // path. Same shape as getSourcePitchDetection.
    ipcMain.handle('audio:getSourceRawPitch', (_event, id: unknown) => {
        if (!audio || typeof audio.getSourceRawPitchDetection !== 'function'
            || !validSourceId(id)) {
            return { frequency: -1, confidence: 0, midiNote: -1, cents: 0, noteName: '' };
        }
        try {
            return audio.getSourceRawPitchDetection(id);
        } catch (e: unknown) {
            console.warn(`[audio] getSourceRawPitch failed: ${e instanceof Error ? e.message : String(e)}`);
            return { frequency: -1, confidence: 0, midiNote: -1, cents: 0, noteName: '' };
        }
    });

    // Raw polyphonic transcription — the ML detector's full active-pitch set.
    // Returns null when the ML detector isn't active (downlevel addon, no ONNX
    // support, or no model loaded) so the renderer feature-detects and falls
    // back to the getPitchDetection / scoreChord path.
    ipcMain.handle('audio:detectNotes', () => {
        if (!audio || typeof audio.detectNotes !== 'function') return null;
        try {
            return audio.detectNotes();
        } catch (e: unknown) {
            console.warn(`[audio] detectNotes failed: ${e instanceof Error ? e.message : String(e)}`);
            return null;
        }
    });

    // ── VST Scanning ───────────────────────────────────────────────────────

    ipcMain.handle('audio:scanPlugins', async (_event, dirs?: string[]) => {
        if (!audio) return [];
        return await audio.scanPlugins(dirs);
    });

    ipcMain.handle('audio:getKnownPlugins', () => {
        return audio?.getKnownPlugins() ?? [];
    });

    ipcMain.handle('audio:savePluginList', (_event, filePath: string) => {
        audio?.savePluginList(filePath || path.join(configDir, 'known-plugins.xml'));
    });

    ipcMain.handle('audio:loadPluginList', (_event, filePath: string) => {
        audio?.loadPluginList(filePath || path.join(configDir, 'known-plugins.xml'));
    });

    // ── Signal Chain ───────────────────────────────────────────────────────

    ipcMain.handle('audio:loadVST', async (_event, pluginPath: string) => {
        // Bracket the in-process load with the crash sentinel. The native
        // loadVST is now a Napi::AsyncWorker that returns a Promise<number>;
        // a hard abort during the load means the awaiting handler never
        // resumes and the sentinel survives for the next startup. Any
        // normal resolve OR a thrown JS exception (the worker rejects on a
        // required-sandbox spawn failure) means the process survived, so
        // disarm in `finally` to avoid a false blocklist entry.
        armSentinel(pluginPath, 'load');
        let slotId = -1;
        try {
            slotId = (await audio?.loadVST(pluginPath)) ?? -1;
        } finally {
            disarmSentinel();
        }
        if (slotId >= 0) vstSlotPaths.set(slotId, pluginPath);
        return slotId;
    });

    ipcMain.handle('audio:loadNAMModel', async (_event, modelPath: string) => {
        return await audio?.loadNAMModel(modelPath) ?? -1;
    });

    ipcMain.handle('audio:loadIR', async (_event, irPath: string) => {
        return await audio?.loadIR(irPath) ?? -1;
    });

    ipcMain.handle('audio:replaceIR', async (_event, slotId: number, irPath: string, gain?: number) => {
        return await audio?.replaceIR(slotId, irPath, typeof gain === 'number' ? gain : -1) ?? false;
    });

    ipcMain.handle('audio:removeProcessor', (_event, slotId: number) => {
        audio?.removeProcessor(slotId);
        vstSlotPaths.delete(slotId);
    });

    ipcMain.handle('audio:moveProcessor', (_event, from: number, to: number) => {
        audio?.moveProcessor(from, to);
    });

    ipcMain.handle('audio:setBypass', (_event, slotId: number, bypassed: boolean) => {
        audio?.setBypass(slotId, bypassed);
    });

    // Stereo routing (St-1/St-2).
    ipcMain.handle('audio:setPan', (_event, slotId: number, pan: number) => {
        audio?.setPan?.(slotId, pan);
    });
    ipcMain.handle('audio:setBranch', (_event, slotId: number, branch: number) => {
        audio?.setBranch?.(slotId, branch);
    });
    ipcMain.handle('audio:setPostGain', (_event, slotId: number, gain: number) => {
        audio?.setPostGain?.(slotId, gain);
    });
    ipcMain.handle('audio:setBranchSrc', (_event, slotId: number, src: number) => {
        audio?.setBranchSrc?.(slotId, src);
    });

    ipcMain.handle('audio:clearChain', () => {
        audio?.clearChain();
        vstSlotPaths.clear();
    });

    ipcMain.handle('audio:getChainState', () => {
        return audio?.getChainState() ?? [];
    });

    // ── Plugin Editor ──────────────────────────────────────────────────────

    ipcMain.handle('audio:openPluginEditor', (_event, slotId: number) => {
        // Editor creation is the common in-process fault point (an editor
        // that must run on the OS main thread). Arm the sentinel with the
        // slot's plugin path before opening; armEditorSentinel self-clears
        // after a grace window since editor creation is asynchronous and has
        // no synchronous success signal. The path comes from the loadVST map
        // first; getChainState is only a fallback for slots created another
        // way (e.g. preset restore).
        let pluginPath = vstSlotPaths.get(slotId);
        if (!pluginPath) {
            const slot = (audio?.getChainState() ?? []).find((s: any) => s?.id === slotId);
            if (slot && typeof slot.path === 'string') pluginPath = slot.path;
        }
        if (pluginPath) armEditorSentinel(pluginPath);
        let opened = false;
        try {
            opened = audio?.openPluginEditor(slotId) ?? false;
        } catch (e) {
            // A thrown call is a clean failure, not a hard crash — disarm so
            // the plugin isn't falsely blocklisted on next startup.
            disarmSentinel();
            throw e;
        }
        // A synchronous false means no editor window was created (the plugin
        // has none, or the open failed cleanly) — nothing can fault, so clear
        // the sentinel now instead of waiting out the grace window. On a
        // true return the sentinel stays armed: the editor is created
        // asynchronously and could still fault within the grace window.
        if (!opened) disarmSentinel();
        return opened;
    });

    ipcMain.handle('audio:closePluginEditor', (_event, slotId: number) => {
        return audio?.closePluginEditor(slotId) ?? false;
    });

    // ── Parameters ─────────────────────────────────────────────────────────

    ipcMain.handle('audio:getParameters', (_event, slotId: number) => {
        return audio?.getParameters(slotId) ?? [];
    });

    ipcMain.handle('audio:setParameter', (_event, slotId: number, paramIndex: number, value: number) => {
        audio?.setParameter(slotId, paramIndex, value);
    });

    ipcMain.handle('audio:setSlotState', (_event, slotId: number, base64State: string): boolean => {
        // typeof-guarded so a downlevel addon is a no-op rather than a thrown
        // IPC error (Constitution VII fail-soft). Returns true when the native
        // addon supports the call (feature-detect signal — the preload always
        // exposes the method, so a renderer-side typeof check cannot tell a
        // downlevel addon apart). try/catch so an addon-side throw resolves
        // to false rather than rejecting the renderer's ipcRenderer.invoke.
        if (audio && typeof audio.setSlotState === 'function') {
            try {
                audio.setSlotState(slotId, base64State);
                return true;
            } catch (err) {
                console.warn('[audio-bridge] setSlotState threw:', err);
                return false;
            }
        }
        return false;
    });

    // ── MIDI ───────────────────────────────────────────────────────────────

    ipcMain.handle('audio:sendMidiToSlot', (_event, slotId: number, msgType: number, channel: number, param1: number, param2?: number) => {
        return audio?.sendMidiToSlot(slotId, msgType, channel, param1, param2 ?? 0) ?? false;
    });

    // ── Backing Track ──────────────────────────────────────────────────────

    ipcMain.handle('audio:loadBackingTrack', (_event, filePath: string) => {
        return audio?.loadBackingTrack(filePath) ?? false;
    });

    ipcMain.handle('audio:startBacking', () => audio?.startBacking());
    ipcMain.handle('audio:stopBacking', () => audio?.stopBacking());
    ipcMain.handle('audio:seekBacking', (_event, seconds: number) => audio?.seekBacking(seconds));
    ipcMain.handle('audio:getBackingPosition', () => audio?.getBackingPosition() ?? 0);
    ipcMain.handle('audio:getBackingDuration', () => audio?.getBackingDuration() ?? 0);
    ipcMain.handle('audio:isBackingPlaying', () => audio?.isBackingPlaying() ?? false);
    ipcMain.handle('audio:setBackingSpeed', (_event, speed: number) => {
        // Feature-detect the method the same way getSampleRate / scoreChord do —
        // a downlevel native addon that predates setBackingSpeed would throw rather
        // than silently no-op, so we guard the method's existence explicitly and
        // return false (the standard "not available" sentinel used across this
        // bridge) so the renderer can handle version-skew without an IPC crash.
        if (!audio || typeof audio.setBackingSpeed !== 'function') return false;
        // Validate before calling into the native layer: AudioEngine::setBackingSpeed
        // silently ignores non-finite / <= 0 values, so an invalid call would return
        // true here but have no effect — return false so the renderer can distinguish
        // a rejected call from a successful one.
        if (!Number.isFinite(speed) || speed <= 0) return false;
        try {
            audio.setBackingSpeed(speed);
            return true;
        } catch (e: unknown) {
            console.warn(`[audio] setBackingSpeed failed: ${e instanceof Error ? e.message : String(e)}`);
            return false;
        }
    });

    // ── Presets ────────────────────────────────────────────────────────────

    ipcMain.handle('audio:savePreset', () => {
        return audio?.savePreset() ?? null;
    });

    ipcMain.handle('audio:loadPreset', async (_event, presetJson: string) => {
        const result = await audio?.loadPreset(presetJson) ?? { success: false, error: 'No audio' };
        // loadPreset rebuilds the native chain from scratch, so the cached
        // slotId→path map no longer reflects it. Clear it — openPluginEditor
        // then falls back to the live getChainState lookup for these slots
        // rather than trusting a stale (possibly id-reused) entry.
        vstSlotPaths.clear();
        return result;
    });

    ipcMain.handle('audio:setMultiBypass', (_event, changes: Array<{slotId: number, bypassed: boolean}>) => {
        return audio?.setMultiBypass(changes) ?? false;
    });

    // ── Audio-effects executor ─────────────────────────────────────────────

    ipcMain.handle('audio-effects:loadChainPlan', async (_event, request: unknown) => {
        vstSlotPaths.clear();
        return await audioEffects.loadChainPlan(request);
    });

    ipcMain.handle('audio-effects:releaseRoute', async (_event, request: unknown) => {
        vstSlotPaths.clear();
        return await audioEffects.releaseRoute(request);
    });

    ipcMain.handle('audio-effects:inspectRoute', (_event, routeKey?: string) => {
        return audioEffects.inspectRoute(routeKey);
    });

    ipcMain.handle('audio-effects:activateSegment', (_event, request: unknown) => {
        return audioEffects.activateSegment(request);
    });

    ipcMain.handle('audio-effects:setStageBypass', (_event, request: unknown) => {
        return audioEffects.setStageBypass(request);
    });

    ipcMain.handle('audio-effects:setStageParameter', (_event, request: unknown) => {
        return audioEffects.setStageParameter(request);
    });

    ipcMain.handle('audio-effects:setRouteGain', (_event, request: unknown) => {
        return audioEffects.setRouteGain(request);
    });
}

export function shutdownAudio(): void {
    if (audio) {
        try {
            audio.shutdown();
            try { console.log('[audio] Engine shut down'); } catch { /* console may be gone */ }
        } catch { /* silent fail during shutdown */ }
        audio = null;
    }
}
