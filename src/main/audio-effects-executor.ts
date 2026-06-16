import * as fs from 'fs';
import * as path from 'path';

const PLAN_SCHEMA = 'slopsmith.audio_effects.chain_plan.v1';
const DEFAULT_ROUTE_KEY = 'desktop-main';
const MAX_STAGES = 24;
const MAX_SEGMENTS = 80;
const MAX_PARAM_INDEX = 4095;
const MAX_SEQUENTIAL_NAM = 8;
const VALID_KINDS = new Set(['nam', 'ir', 'vst', 'utility', 'bypass']);
const VALID_ROLES = new Set(['input', 'pre-pedal', 'pedal', 'amp', 'post-pedal', 'rack', 'cab', 'master-pre', 'master-post', 'utility', 'unknown']);
const AUTHORIZATIONS = new Set(['user-action', 'restore-selection', 'playback-session']);

const NATIVE_TYPES: Record<string, number> = {
    vst: 0,
    nam: 1,
    ir: 2,
};

type Dict = Record<string, unknown>;

type AudioEffectsNativeAudio = {
    loadPreset?: (presetJson: string) => Promise<unknown> | unknown;
    savePreset?: () => unknown;
    clearChain?: () => Promise<unknown> | unknown;
    getChainState?: () => unknown;
    setBypass?: (slotId: number, bypassed: boolean) => unknown;
    setMultiBypass?: (changes: Array<{ slotId: number; bypassed: boolean }>) => unknown;
    setParameter?: (slotId: number, paramIndex: number, value: number) => unknown;
    setGain?: (which: string, value: number) => Promise<unknown> | unknown;
    setMonitorMute?: (muted: boolean) => Promise<unknown> | unknown;
    setMonitorMuteSuppressed?: (suppressed: boolean) => Promise<unknown> | unknown;
    isMonitorMuted?: () => Promise<unknown> | unknown;
    startAudio?: () => Promise<unknown> | unknown;
};

type NativeAudioGetter = () => AudioEffectsNativeAudio | null;

type ValidStage = {
    stageId: string;
    kind: string;
    role: string;
    assetRef: string;
    stateRef: string;
    bypassed: boolean;
    gainDb: number;
    native: boolean;
};

type ValidSegment = {
    segmentId: string;
    stageIds: string[];
    stageBypass: Record<string, boolean>;
};

type ValidPlan = {
    planId: string;
    routeKey: string;
    providerId: string;
    stages: ValidStage[];
    segments: ValidSegment[];
};

type RouteGains = {
    input?: number;
    chain?: number;
};

type LoadOptions = {
    preloadMute: {
        enabled: boolean;
        dryDuringLoad: boolean;
        targetGain: number;
        holdMs: number;
    } | null;
    gains: RouteGains;
    startAudio: boolean;
};

type RouteState = {
    routeKey: string;
    providerId: string;
    planId: string;
    state: string;
    activeSegmentId: string;
    stageSlots: Map<string, number>;
    stageKinds: Map<string, string>;
    segments: ValidSegment[];
    loadedAt: string;
    updatedAt: string;
    lastOutcome: SafeOutcome | null;
};

type AudioEffectsOutcome = 'handled' | 'degraded' | 'failed' | 'unavailable' | 'no-target' | 'user-action-required';

type SafeOutcome = {
    outcome: AudioEffectsOutcome;
    status: string;
    reason: string;
    payload?: Dict;
};

function asRecord(value: unknown): Dict | null {
    return value && typeof value === 'object' && !Array.isArray(value) ? value as Dict : null;
}

function asArray(value: unknown): unknown[] {
    return Array.isArray(value) ? value : [];
}

function now(): string {
    return new Date().toISOString();
}

function bounded(value: unknown, max = 200): string {
    return String(value ?? '')
        .replace(/(?:\/Users\/|\/home\/|\/root\b\/?)[^\r\n\t"'`,;(){}\[\]<>|]*/g, '[path]')
        .replace(/[A-Za-z]:\\[^\r\n\t"'`,;(){}\[\]<>|]*/g, '[path]')
        .replace(/https?:\/\/[^\s?#]+[^\s]*/gi, '[url]')
        .replace(/file:\/\/[^\s]+/gi, '[path]')
        .replace(/\b(token|secret|password|api[_-]?key|key)=([^\s&]+)/gi, '$1=[redacted]')
        .replace(/\b[^\s]+\.(psarc|sloppak|wem|ogg|mp3|wav|flac|nam|vst3|component|dll|json|db)\b/gi, '[file]')
        .replace(/\s+/g, ' ')
        .trim()
        .slice(0, max);
}

function safeId(value: unknown, fallback: string): string {
    const text = String(value ?? '').trim() || fallback;
    return text.replace(/[^A-Za-z0-9_.:-]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 96) || fallback;
}

function safeNumber(value: unknown, fallback = 0): number {
    const numberValue = Number(value);
    return Number.isFinite(numberValue) ? numberValue : fallback;
}

function clampGain(value: unknown, fallback = Number.NaN): number {
    const numberValue = safeNumber(value, fallback);
    return Number.isFinite(numberValue) ? Math.max(0, Math.min(32, numberValue)) : Number.NaN;
}

function parseGains(value: unknown): RouteGains {
    const input = asRecord(value) || {};
    const gains: RouteGains = {};
    const inputGain = clampGain(input.input);
    const chainGain = clampGain(input.chain);
    if (Number.isFinite(inputGain)) gains.input = inputGain;
    if (Number.isFinite(chainGain)) gains.chain = chainGain;
    return gains;
}

function parseLoadOptions(value: unknown): LoadOptions {
    const input = asRecord(value) || {};
    const rawPreload = asRecord(input.preloadMute) || asRecord(input.preLoadMute) || asRecord(input.loadMute) || asRecord(input.preload) || {};
    const hasPreload = input.preloadMute === true || input.loadMute === true || Object.keys(rawPreload).length > 0;
    const targetGain = clampGain(rawPreload.targetGain ?? rawPreload.restoreGain ?? rawPreload.chainGain, 1);
    return {
        preloadMute: hasPreload ? {
            enabled: rawPreload.enabled !== false && input.preloadMute !== false && input.loadMute !== false,
            dryDuringLoad: rawPreload.dryDuringLoad !== false,
            targetGain: Number.isFinite(targetGain) ? targetGain : 1,
            holdMs: Math.max(0, Math.min(5000, Math.round(safeNumber(rawPreload.holdMs, 0)))),
        } : null,
        gains: parseGains(input.gains ?? input.gain),
        startAudio: input.startAudio === true,
    };
}

function parseStrictNumber(value: unknown): number {
    if (typeof value === 'number') return Number.isFinite(value) ? value : Number.NaN;
    if (typeof value !== 'string') return Number.NaN;
    const trimmed = value.trim();
    if (!trimmed || !/^[+-]?(?:\d+(?:\.\d*)?|\.\d+)$/.test(trimmed)) return Number.NaN;
    const parsed = Number(trimmed);
    return Number.isFinite(parsed) ? parsed : Number.NaN;
}

function parseParamIndex(input: Dict): number {
    const paramIndex = parseStrictNumber(input.paramIndex);
    if (Number.isFinite(paramIndex)) return paramIndex;
    return parseStrictNumber(input.parameterId);
}

function nativeFailure(result: unknown): boolean {
    return result === false || asRecord(result)?.success === false;
}

function safeBool(value: unknown, fallback = false): boolean {
    return value === true || value === false ? value : fallback;
}

function safeOutcome(outcome: AudioEffectsOutcome, reason: unknown, payload?: Dict, status?: string): SafeOutcome {
    return {
        outcome,
        status: status || outcome,
        reason: bounded(reason),
        ...(payload ? { payload } : {}),
    };
}

function safeRoute(route: RouteState): Dict {
    return {
        routeKey: route.routeKey,
        providerId: route.providerId,
        planId: route.planId,
        state: route.state,
        activeSegmentId: route.activeSegmentId,
        nativeStageCount: route.stageSlots.size,
        stageKinds: Array.from(route.stageKinds.values()),
        segmentCount: route.segments.length,
        loadedAt: route.loadedAt,
        updatedAt: route.updatedAt,
        lastOutcome: route.lastOutcome ? {
            outcome: route.lastOutcome.outcome,
            status: route.lastOutcome.status,
            reason: route.lastOutcome.reason,
        } : null,
    };
}

function normalizeAssetMap(value: unknown): Map<string, Dict> {
    const map = new Map<string, Dict>();
    const record = asRecord(value);
    if (!record) return map;
    for (const [key, entry] of Object.entries(record)) {
        const asset = asRecord(entry);
        if (asset) map.set(key, asset);
    }
    return map;
}

function validateAssetPath(filePath: unknown, kind: string, errors: string[], stageId: string): string {
    const candidate = String(filePath ?? '').trim();
    if (!candidate) {
        errors.push(`Stage ${stageId} has no trusted asset path`);
        return '';
    }
    if (/^(?:https?:|file:)/i.test(candidate) || !path.isAbsolute(candidate)) {
        errors.push(`Stage ${stageId} asset path must be an absolute local path inside the trusted executor call`);
        return '';
    }
    const ext = path.extname(candidate).toLowerCase();
    const validExt = kind === 'nam'
        ? ext === '.nam'
        : kind === 'ir'
            ? ['.wav', '.flac', '.aiff', '.aif'].includes(ext)
            : kind === 'vst'
                ? ['.vst3', '.component', '.dll'].includes(ext)
                : true;
    if (!validExt) errors.push(`Stage ${stageId} asset extension is not valid for ${kind}`);
    if (!fs.existsSync(candidate)) errors.push(`Stage ${stageId} trusted asset is missing`);
    return candidate;
}

function validatePlan(request: unknown): { ok: true; plan: ValidPlan; presetJson: string } | { ok: false; errors: string[] } {
    const input = asRecord(request) || {};
    const planInput = asRecord(input.plan) || asRecord(input.chainPlan) || null;
    const errors: string[] = [];
    if (!planInput) return { ok: false, errors: ['Missing audio-effects chain plan'] };

    const authorization = String(input.authorization ?? '').trim();
    if (!AUTHORIZATIONS.has(authorization)) {
        errors.push('Audio-effects plan loading requires user-action, restore-selection, or playback-session authorization');
    }

    const schema = String(planInput.schema ?? '').trim();
    if (schema !== PLAN_SCHEMA) errors.push('Unsupported audio-effects chain plan schema');

    const routeKey = safeId(planInput.routeKey ?? planInput.route ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
    const providerId = safeId(planInput.providerId, 'provider');
    const planId = safeId(planInput.planId ?? planInput.chainId, 'plan');
    const rawStages = asArray(planInput.stages);
    if (rawStages.length < 1) errors.push('Audio-effects chain plan must include at least one stage');
    if (rawStages.length > MAX_STAGES) errors.push(`Audio-effects chain plan exceeds maximum stage count ${MAX_STAGES}`);

    const assets = normalizeAssetMap(input.assets ?? input.trustedAssets);
    const states = normalizeAssetMap(input.states ?? input.trustedStates);
    const stages: ValidStage[] = [];
    const nativePresetChain: Dict[] = [];
    const seenStageIds = new Set<string>();
    let sequentialNam = 0;

    rawStages.slice(0, MAX_STAGES).forEach((entry, index) => {
        const stageInput = asRecord(entry) || {};
        const kind = safeId(stageInput.kind, 'utility');
        const roleCandidate = safeId(stageInput.role ?? stageInput.slot, 'unknown');
        const role = VALID_ROLES.has(roleCandidate) ? roleCandidate : 'unknown';
        const stageId = safeId(stageInput.stageId ?? stageInput.id ?? `${kind}-${index}`, `stage-${index}`);
        if (seenStageIds.has(stageId)) {
            errors.push(`Duplicate stageId ${stageId}`);
            return;
        }
        seenStageIds.add(stageId);
        const assetRef = String(stageInput.assetRef ?? stageInput.ref ?? '').trim();
        const stateRef = String(stageInput.stateRef ?? '').trim();
        const native = kind === 'nam' || kind === 'ir' || kind === 'vst';

        if (!VALID_KINDS.has(kind)) errors.push(`Stage ${stageId} has unsupported kind`);
        if (native && !assetRef) errors.push(`Stage ${stageId} requires an opaque assetRef`);
        if (kind === 'nam') sequentialNam += 1;
        else sequentialNam = 0;
        if (sequentialNam > MAX_SEQUENTIAL_NAM) errors.push(`Plan exceeds maximum sequential NAM count ${MAX_SEQUENTIAL_NAM}`);

        const stage: ValidStage = {
            stageId,
            kind: VALID_KINDS.has(kind) ? kind : 'utility',
            role,
            assetRef,
            stateRef,
            bypassed: safeBool(stageInput.bypassed, false),
            gainDb: safeNumber(stageInput.gainDb, 0),
            native,
        };
        stages.push(stage);

        if (!native) return;

        const asset = assets.get(assetRef);
        if (!asset) {
            errors.push(`Stage ${stageId} has no trusted asset for its assetRef`);
            return;
        }
        const assetKind = safeId(asset.kind, kind);
        if (assetKind !== kind) errors.push(`Stage ${stageId} trusted asset kind does not match plan kind`);
        const assetPath = validateAssetPath(asset.path, kind, errors, stageId);
        const state = stateRef ? states.get(stateRef) : null;
        const stateBase64 = String(asset.stateBase64 ?? state?.stateBase64 ?? state?.base64 ?? '').trim();
        const nativeStage: Dict = {
            type: NATIVE_TYPES[kind],
            name: bounded(asset.safeName ?? asset.label ?? `${role}-${kind}`, 96) || `${role}-${kind}`,
            path: assetPath,
            bypassed: stage.bypassed,
        };
        if (stateBase64) nativeStage.state = stateBase64;
        nativePresetChain.push(nativeStage);
    });

    const nativeStageIds = stages.filter((stage) => stage.native).map((stage) => stage.stageId);
    const seenSegmentIds = new Set<string>();
    const segments: ValidSegment[] = asArray(planInput.segments).slice(0, MAX_SEGMENTS).map((segment, index) => {
        const item = asRecord(segment) || {};
        // segmentId is the public lookup key; activateSegment() resolves it with Array.find,
        // so a duplicate would make the later segment unreachable. Reject the plan instead.
        const segmentId = safeId(item.segmentId ?? item.toneKey ?? item.id ?? `segment-${index}`, `segment-${index}`);
        if (seenSegmentIds.has(segmentId)) errors.push(`Audio-effects chain plan has a duplicate segmentId: ${segmentId}`);
        seenSegmentIds.add(segmentId);
        const rawStageBypass = asRecord(item.stageBypass) || asRecord(item.stageBypasses) || asRecord(item.bypassByStage) || {};
        const stageBypass: Record<string, boolean> = {};
        for (const [stageId, bypassed] of Object.entries(rawStageBypass)) {
            const safeStageId = safeId(stageId, '');
            if (nativeStageIds.includes(safeStageId)) stageBypass[safeStageId] = safeBool(bypassed, false);
        }
        return {
            segmentId,
            stageIds: asArray(item.stageIds ?? item.stages).map((value) => safeId(value, '')).filter((value) => nativeStageIds.includes(value)),
            stageBypass,
        };
    });

    if (nativePresetChain.length < 1) errors.push('Audio-effects chain plan has no loadable native stages');

    if (errors.length) return { ok: false, errors };
    return {
        ok: true,
        plan: { planId, routeKey, providerId, stages, segments },
        presetJson: JSON.stringify({ chain: nativePresetChain }),
    };
}

function normalizeLoadResult(value: unknown): { success: boolean; slotsLoaded: number; error: string } {
    const record = asRecord(value);
    if (!record) return { success: false, slotsLoaded: 0, error: 'Native load returned an unsupported result' };
    return {
        success: record.success === true,
        slotsLoaded: safeNumber(record.slotsLoaded, 0),
        error: bounded(record.error ?? ''),
    };
}

function chainSlots(nativeAudio: AudioEffectsNativeAudio | null): Dict[] {
    if (!nativeAudio || typeof nativeAudio.getChainState !== 'function') return [];
    const state = nativeAudio.getChainState();
    return asArray(state).map((entry) => asRecord(entry)).filter((entry): entry is Dict => !!entry);
}

async function restorePreset(nativeAudio: AudioEffectsNativeAudio, presetJson: unknown): Promise<boolean> {
    if (typeof presetJson !== 'string' || !presetJson.trim() || typeof nativeAudio.loadPreset !== 'function') return false;
    try {
        await nativeAudio.loadPreset(presetJson);
        return true;
    } catch (_) {
        return false;
    }
}

async function readMonitorMuted(nativeAudio: AudioEffectsNativeAudio): Promise<boolean | null> {
    if (typeof nativeAudio.isMonitorMuted !== 'function') return null;
    try {
        return Boolean(await nativeAudio.isMonitorMuted());
    } catch (_) {
        return null;
    }
}

async function trySetMonitorMute(nativeAudio: AudioEffectsNativeAudio, muted: boolean): Promise<void> {
    if (typeof nativeAudio.setMonitorMute !== 'function') return;
    try { await nativeAudio.setMonitorMute(muted); } catch (_) { /* best effort */ }
}

async function trySetMonitorMuteSuppressed(nativeAudio: AudioEffectsNativeAudio, suppressed: boolean): Promise<void> {
    if (typeof nativeAudio.setMonitorMuteSuppressed !== 'function') return;
    try { await nativeAudio.setMonitorMuteSuppressed(suppressed); } catch (_) { /* best effort */ }
}

async function trySetGain(nativeAudio: AudioEffectsNativeAudio, which: string, value: number): Promise<boolean> {
    if (typeof nativeAudio.setGain !== 'function' || !Number.isFinite(value)) return false;
    try {
        const result = await nativeAudio.setGain(which, value);
        return !nativeFailure(result);
    } catch (_) {
        return false;
    }
}

async function applyGains(nativeAudio: AudioEffectsNativeAudio, gains: RouteGains, skipChain = false): Promise<string[]> {
    const failed: string[] = [];
    if (gains.input != null && !(await trySetGain(nativeAudio, 'input', gains.input))) failed.push('input');
    if (!skipChain && gains.chain != null && !(await trySetGain(nativeAudio, 'chain', gains.chain))) failed.push('chain');
    return failed;
}

function schedulePreloadRestore(nativeAudio: AudioEffectsNativeAudio, previousMonitorMute: boolean | null, targetGain: number, holdMs: number, shouldRestore?: () => boolean): void {
    const restore = async () => {
        if (shouldRestore && !shouldRestore()) return;
        if (previousMonitorMute !== null) await trySetMonitorMute(nativeAudio, previousMonitorMute);
        const restoreTarget = clampGain(targetGain, 1);
        const steps = [restoreTarget * 0.25, restoreTarget * 0.5, restoreTarget * 0.8, restoreTarget];
        for (const value of steps) {
            if (shouldRestore && !shouldRestore()) return;
            await trySetGain(nativeAudio, 'chain', value);
            await new Promise((resolve) => setTimeout(resolve, 6));
        }
    };
    setTimeout(() => { void restore(); }, Math.max(0, holdMs));
}

export function createAudioEffectsExecutor(getAudio: NativeAudioGetter) {
    const routes = new Map<string, RouteState>();
    let preloadRestoreVersion = 0;

    function updateOutcome(route: RouteState, outcome: SafeOutcome): SafeOutcome {
        route.lastOutcome = outcome;
        route.updatedAt = now();
        return outcome;
    }

    async function loadChainPlan(request: unknown): Promise<SafeOutcome> {
        const nativeAudio = getAudio();
        if (!nativeAudio || typeof nativeAudio.loadPreset !== 'function') {
            return safeOutcome('unavailable', 'Native audio engine is unavailable');
        }
        const input = asRecord(request) || {};
        const options = parseLoadOptions(input.options ?? input.executorOptions ?? input.loadOptions);
        const validation = validatePlan(request);
        if (!validation.ok) {
            return safeOutcome('failed', 'Audio-effects chain plan validation failed', { errors: validation.errors.map((error) => bounded(error)) });
        }

        const started = Date.now();
        const restoreVersion = ++preloadRestoreVersion;
        const rollbackPreset = typeof nativeAudio.savePreset === 'function' ? nativeAudio.savePreset() : null;
        let previousMonitorMute: boolean | null = null;
        if (options.preloadMute?.enabled) {
            previousMonitorMute = await readMonitorMuted(nativeAudio);
            await trySetGain(nativeAudio, 'chain', 0);
            await trySetMonitorMute(nativeAudio, options.preloadMute.dryDuringLoad ? false : true);
        }
        let result: { success: boolean; slotsLoaded: number; error: string };
        try {
            result = normalizeLoadResult(await nativeAudio.loadPreset(validation.presetJson));
        } catch (error) {
            const rollbackApplied = await restorePreset(nativeAudio, rollbackPreset);
            if (options.preloadMute?.enabled) schedulePreloadRestore(nativeAudio, previousMonitorMute, options.gains.chain ?? options.preloadMute.targetGain, 0, () => restoreVersion === preloadRestoreVersion);
            return safeOutcome('failed', 'Native audio-effects plan load threw', { error: bounded(error instanceof Error ? error.message : String(error)), rollbackApplied });
        }

        const nativeStages = validation.plan.stages.filter((stage) => stage.native);
        if (!result.success) {
            const rollbackApplied = await restorePreset(nativeAudio, rollbackPreset);
            if (options.preloadMute?.enabled) schedulePreloadRestore(nativeAudio, previousMonitorMute, options.gains.chain ?? options.preloadMute.targetGain, 0, () => restoreVersion === preloadRestoreVersion);
            return safeOutcome('failed', 'Native audio-effects plan load failed', {
                routeKey: validation.plan.routeKey,
                providerId: validation.plan.providerId,
                planId: validation.plan.planId,
                stageCount: nativeStages.length,
                slotsLoaded: result.slotsLoaded,
                loadMs: Date.now() - started,
                error: result.error,
                rollbackApplied,
            });
        }

        if (result.slotsLoaded < nativeStages.length) {
            const rollbackApplied = await restorePreset(nativeAudio, rollbackPreset);
            if (options.preloadMute?.enabled) schedulePreloadRestore(nativeAudio, previousMonitorMute, options.gains.chain ?? options.preloadMute.targetGain, 0, () => restoreVersion === preloadRestoreVersion);
            return safeOutcome('degraded', 'Native audio-effects plan partially loaded and was rolled back', {
                routeKey: validation.plan.routeKey,
                providerId: validation.plan.providerId,
                planId: validation.plan.planId,
                stageCount: nativeStages.length,
                slotsLoaded: result.slotsLoaded,
                loadMs: Date.now() - started,
                rollbackApplied,
            });
        }

        let slots: Dict[];
        try {
            slots = chainSlots(nativeAudio);
        } catch (error) {
            const rollbackApplied = await restorePreset(nativeAudio, rollbackPreset);
            if (options.preloadMute?.enabled) schedulePreloadRestore(nativeAudio, previousMonitorMute, options.gains.chain ?? options.preloadMute.targetGain, 0, () => restoreVersion === preloadRestoreVersion);
            return safeOutcome('failed', 'Native chain-state lookup threw', {
                routeKey: validation.plan.routeKey,
                providerId: validation.plan.providerId,
                planId: validation.plan.planId,
                error: bounded(error instanceof Error ? error.message : String(error)),
                rollbackApplied,
            });
        }
        const stageSlots = new Map<string, number>();
        const stageKinds = new Map<string, string>();
        nativeStages.forEach((stage, index) => {
            const slotId = safeNumber(slots[index]?.id, -1);
            if (slotId >= 0) stageSlots.set(stage.stageId, slotId);
            stageKinds.set(stage.stageId, stage.kind);
        });
        // Every native stage must map to a real slot; an incomplete mapping would report the load
        // as handled while later stage operations silently return no-target. Roll back instead.
        if (stageSlots.size !== nativeStages.length) {
            const rollbackApplied = await restorePreset(nativeAudio, rollbackPreset);
            if (options.preloadMute?.enabled) schedulePreloadRestore(nativeAudio, previousMonitorMute, options.gains.chain ?? options.preloadMute.targetGain, 0, () => restoreVersion === preloadRestoreVersion);
            return safeOutcome('degraded', 'Native slot mapping was incomplete and was rolled back', {
                routeKey: validation.plan.routeKey,
                providerId: validation.plan.providerId,
                planId: validation.plan.planId,
                stageCount: nativeStages.length,
                slotsMapped: stageSlots.size,
                rollbackApplied,
            });
        }

        const route: RouteState = {
            routeKey: validation.plan.routeKey,
            providerId: validation.plan.providerId,
            planId: validation.plan.planId,
            state: result.slotsLoaded >= nativeStages.length ? 'loaded' : 'degraded',
            activeSegmentId: '',
            stageSlots,
            stageKinds,
            segments: validation.plan.segments,
            loadedAt: now(),
            updatedAt: now(),
            lastOutcome: null,
        };
        routes.set(route.routeKey, route);
        const gainFailures = await applyGains(nativeAudio, options.gains, options.preloadMute?.enabled === true);
        if (options.startAudio && typeof nativeAudio.startAudio === 'function') {
            try { await nativeAudio.startAudio(); } catch (_) { /* load succeeded; start is best-effort */ }
        }
        if (options.preloadMute?.enabled) {
            schedulePreloadRestore(nativeAudio, previousMonitorMute, options.gains.chain ?? options.preloadMute.targetGain, options.preloadMute.holdMs, () => {
                const current = routes.get(validation.plan.routeKey);
                return restoreVersion === preloadRestoreVersion && current?.planId === validation.plan.planId;
            });
        }
        return updateOutcome(route, safeOutcome('handled', 'Audio-effects chain plan loaded', {
            route: safeRoute(route),
            stageCount: nativeStages.length,
            slotsLoaded: result.slotsLoaded,
            loadMs: Date.now() - started,
            gainFailures,
        }));
    }

    function inspectRoute(routeKeyInput?: unknown): SafeOutcome {
        const routeKey = safeId(routeKeyInput ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
        const route = routes.get(routeKey);
        if (!route) return safeOutcome('no-target', 'No audio-effects route has been loaded', { routeKey });
        return safeOutcome('handled', 'Audio-effects route inspected', { route: safeRoute(route) });
    }

    async function releaseRoute(request: unknown): Promise<SafeOutcome> {
        const input = asRecord(request) || {};
        const routeKey = safeId(input.routeKey ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
        const route = routes.get(routeKey);
        if (!route) return safeOutcome('no-target', 'No audio-effects route has been loaded', { routeKey });
        const nativeAudio = getAudio();
        if (!nativeAudio || typeof nativeAudio.clearChain !== 'function') return updateOutcome(route, safeOutcome('unavailable', 'Native route release is unavailable', { routeKey }));
        preloadRestoreVersion += 1;
        const cleanupFailures: string[] = [];
        if (!(await trySetGain(nativeAudio, 'chain', 0))) cleanupFailures.push('chain-gain');
        let releaseFailure: SafeOutcome | null = null;
        try {
            const result = await nativeAudio.clearChain();
            if (nativeFailure(result)) releaseFailure = safeOutcome('failed', 'Native route release returned failure', { routeKey });
        } catch (error) {
            releaseFailure = safeOutcome('failed', 'Native route release threw', { routeKey, error: bounded(error instanceof Error ? error.message : String(error)) });
        }
        await trySetMonitorMute(nativeAudio, true);
        await trySetMonitorMuteSuppressed(nativeAudio, false);
        if (releaseFailure) return updateOutcome(route, releaseFailure);
        routes.delete(routeKey);
        return safeOutcome('handled', 'Audio-effects route released', { routeKey, providerId: route.providerId, planId: route.planId, cleanupFailures });
    }

    async function setRouteGain(request: unknown): Promise<SafeOutcome> {
        const input = asRecord(request) || {};
        const routeKey = safeId(input.routeKey ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
        const route = routes.get(routeKey);
        if (!route) return safeOutcome('no-target', 'No audio-effects route has been loaded', { routeKey });
        const gains = parseGains(input.gains ?? (input.which ? { [String(input.which)]: input.value } : {}));
        if (gains.input == null && gains.chain == null) return updateOutcome(route, safeOutcome('failed', 'Audio-effects route gain request is invalid', { routeKey }));
        const nativeAudio = getAudio();
        if (!nativeAudio || typeof nativeAudio.setGain !== 'function') return updateOutcome(route, safeOutcome('unavailable', 'Native route gain is unavailable', { routeKey }));
        const failed = await applyGains(nativeAudio, gains);
        if (failed.length) return updateOutcome(route, safeOutcome('failed', 'Native route gain returned failure', { routeKey, failed }));
        return updateOutcome(route, safeOutcome('handled', 'Audio-effects route gain applied', { route: safeRoute(route), gains }));
    }

    async function setStageBypass(request: unknown): Promise<SafeOutcome> {
        const input = asRecord(request) || {};
        const routeKey = safeId(input.routeKey ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
        const stageId = safeId(input.stageId, '');
        const route = routes.get(routeKey);
        if (!route) return safeOutcome('no-target', 'No audio-effects route has been loaded', { routeKey });
        const slotId = route.stageSlots.get(stageId);
        if (slotId == null) return updateOutcome(route, safeOutcome('no-target', 'Audio-effects stage is not mapped to a native slot', { routeKey, stageId }));
        const nativeAudio = getAudio();
        if (!nativeAudio || typeof nativeAudio.setBypass !== 'function') return updateOutcome(route, safeOutcome('unavailable', 'Native stage bypass is unavailable', { routeKey, stageId }));
        try {
            const result = await nativeAudio.setBypass(slotId, safeBool(input.bypassed, false));
            if (nativeFailure(result)) return updateOutcome(route, safeOutcome('failed', 'Native stage bypass returned failure', { routeKey, stageId }));
        } catch (error) {
            return updateOutcome(route, safeOutcome('failed', 'Native stage bypass threw', { routeKey, stageId, error: bounded(error instanceof Error ? error.message : String(error)) }));
        }
        return updateOutcome(route, safeOutcome('handled', 'Audio-effects stage bypass applied', { route: safeRoute(route), stageId }));
    }

    async function setStageParameter(request: unknown): Promise<SafeOutcome> {
        const input = asRecord(request) || {};
        const routeKey = safeId(input.routeKey ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
        const stageId = safeId(input.stageId, '');
        const route = routes.get(routeKey);
        if (!route) return safeOutcome('no-target', 'No audio-effects route has been loaded', { routeKey });
        const slotId = route.stageSlots.get(stageId);
        if (slotId == null) return updateOutcome(route, safeOutcome('no-target', 'Audio-effects stage is not mapped to a native slot', { routeKey, stageId }));
        const paramIndex = parseParamIndex(input);
        const value = parseStrictNumber(input.value);
        if (!Number.isInteger(paramIndex) || paramIndex < 0 || paramIndex > MAX_PARAM_INDEX || !Number.isFinite(value)) {
            return updateOutcome(route, safeOutcome('failed', 'Audio-effects stage parameter request is invalid', { routeKey, stageId }));
        }
        const nativeAudio = getAudio();
        if (!nativeAudio || typeof nativeAudio.setParameter !== 'function') return updateOutcome(route, safeOutcome('unavailable', 'Native stage parameter control is unavailable', { routeKey, stageId }));
        try {
            const result = await nativeAudio.setParameter(slotId, paramIndex, value);
            if (nativeFailure(result)) return updateOutcome(route, safeOutcome('failed', 'Native stage parameter returned failure', { routeKey, stageId, paramIndex }));
        } catch (error) {
            return updateOutcome(route, safeOutcome('failed', 'Native stage parameter threw', { routeKey, stageId, paramIndex, error: bounded(error instanceof Error ? error.message : String(error)) }));
        }
        return updateOutcome(route, safeOutcome('handled', 'Audio-effects stage parameter applied', { route: safeRoute(route), stageId, paramIndex }));
    }

    async function activateSegment(request: unknown): Promise<SafeOutcome> {
        const input = asRecord(request) || {};
        const routeKey = safeId(input.routeKey ?? DEFAULT_ROUTE_KEY, DEFAULT_ROUTE_KEY);
        const segmentId = safeId(input.segmentId ?? input.toneKey, '');
        const route = routes.get(routeKey);
        if (!route) return safeOutcome('no-target', 'No audio-effects route has been loaded', { routeKey });
        const segment = route.segments.find((item) => item.segmentId === segmentId);
        if (!segment) return updateOutcome(route, safeOutcome('no-target', 'Audio-effects segment is not present in the loaded plan', { routeKey, segmentId }));
        const nativeAudio = getAudio();
        if (!nativeAudio || typeof nativeAudio.setMultiBypass !== 'function') return updateOutcome(route, safeOutcome('unavailable', 'Native multi-bypass is unavailable', { routeKey, segmentId }));
        const active = new Set(segment.stageIds);
        const changes = Array.from(route.stageSlots.entries()).map(([stageId, slotId]) => ({
            slotId,
            bypassed: active.has(stageId)
                ? (Object.prototype.hasOwnProperty.call(segment.stageBypass, stageId) ? segment.stageBypass[stageId] : false)
                : true,
        }));
        try {
            const result = await nativeAudio.setMultiBypass(changes);
            if (nativeFailure(result)) return updateOutcome(route, safeOutcome('failed', 'Native multi-bypass returned failure', { routeKey, segmentId, changedCount: changes.length }));
        } catch (error) {
            return updateOutcome(route, safeOutcome('failed', 'Native multi-bypass threw', { routeKey, segmentId, changedCount: changes.length, error: bounded(error instanceof Error ? error.message : String(error)) }));
        }
        route.activeSegmentId = segment.segmentId;
        return updateOutcome(route, safeOutcome('handled', 'Audio-effects segment activated', { route: safeRoute(route), segmentId, changedCount: changes.length }));
    }

    return {
        loadChainPlan,
        releaseRoute,
        inspectRoute,
        activateSegment,
        setStageBypass,
        setStageParameter,
        setRouteGain,
    };
}

export type AudioEffectsExecutor = ReturnType<typeof createAudioEffectsExecutor>;
