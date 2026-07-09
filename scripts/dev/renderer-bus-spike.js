// Renderer-bus capture spike (Phase 2, Option B go/no-go).
//
// Paste into the DevTools console of the running desktop app (the main
// window, so window.feedBackDesktop is present) while the engine is running —
// ideally with the output device in "Windows Audio (Exclusive Mode)".
//
// What it does:
//   1. Enables the engine's renderer bus (setRendererBus).
//   2. Creates an AudioContext routed to a NULL sink (setSinkId {type:'none'})
//      so the graph keeps rendering even when the output endpoint is
//      exclusive-locked and Chromium cannot open it. THIS IS THE LOAD-BEARING
//      TRICK — if it fails on some platform, Option B needs a rethink.
//   3. Runs a 440 Hz tone through an AudioWorklet tap that batches ~10 ms
//      chunks and pushes them over IPC into the engine's ring.
//   4. Logs ring metrics every 5 s. You should HEAR the tone on the exclusive
//      device (it's mixed into the engine output like a backing track).
//
// Reading the numbers:
//   - fillFrames trend  ≈ clock drift: rising = renderer clock fast vs device
//     (will eventually overflow), falling = slow (will underflow). Flat ± a
//     chunk is healthy. fill/sampleRate is the added latency contribution.
//   - underflowCount    : output callbacks that got a short read (audible gap).
//   - overflowCount     : producer lapped the ring (latency reset + audible skip).
//
// Stop with:  await window.__busSpike.stop()

(async () => {
    'use strict';
    const api = window.feedBackDesktop?.audio;
    if (!api?.setRendererBus) { console.error('[bus-spike] renderer-bus API missing (old build?)'); return; }
    if (!(await api.isAudioRunning())) { console.error('[bus-spike] engine not running — start it first'); return; }

    await api.setRendererBus(true, 1.0);

    const ctx = new AudioContext();
    // Null sink: render without an output device. Fall back to default sink if
    // unsupported (then this spike only works in shared mode).
    let sink = 'none';
    try { await ctx.setSinkId({ type: 'none' }); }
    catch (e) { sink = 'default(!)'; console.warn('[bus-spike] setSinkId({type:none}) FAILED — Option B blocker on this platform:', e); }
    if (ctx.state !== 'running') await ctx.resume().catch(() => {});

    const workletCode = `
        class BusTap extends AudioWorkletProcessor {
            process(inputs) {
                const inp = inputs[0];
                if (inp && inp[0]) {
                    const L = inp[0], R = inp[1] || inp[0];
                    const out = new Float32Array(L.length * 2);
                    for (let i = 0; i < L.length; i++) { out[i * 2] = L[i]; out[i * 2 + 1] = R[i]; }
                    this.port.postMessage(out, [out.buffer]);
                }
                return true;
            }
        }
        registerProcessor('bus-tap', BusTap);
    `;
    const modUrl = URL.createObjectURL(new Blob([workletCode], { type: 'application/javascript' }));
    await ctx.audioWorklet.addModule(modUrl);
    const tap = new AudioWorkletNode(ctx, 'bus-tap', { numberOfInputs: 1, channelCount: 2 });

    // Batch 128-frame quanta to ~10 ms before crossing IPC (≈100 sends/s).
    const BATCH_FRAMES = Math.round(ctx.sampleRate / 100);
    let batch = [], batchFrames = 0, sentChunks = 0;
    tap.port.onmessage = (e) => {
        batch.push(e.data);
        batchFrames += e.data.length / 2;
        if (batchFrames >= BATCH_FRAMES) {
            const merged = new Float32Array(batchFrames * 2);
            let o = 0;
            for (const c of batch) { merged.set(c, o); o += c.length; }
            api.pushRendererAudio(merged, ctx.sampleRate);
            sentChunks++;
            batch = []; batchFrames = 0;
        }
    };

    const osc = ctx.createOscillator();
    osc.frequency.value = 440;
    const gain = ctx.createGain();
    gain.gain.value = 0.1;
    osc.connect(gain).connect(tap);
    tap.connect(ctx.destination);   // keeps the graph pulled; tap outputs silence
    osc.start();

    console.log(`[bus-spike] running — ctx.sampleRate=${ctx.sampleRate} sink=${sink} state=${ctx.state} batch=${BATCH_FRAMES}f`);

    let last = null, lastT = performance.now(), fill0 = null, t0 = null;
    const timer = setInterval(async () => {
        const m = await api.getRendererBusMetrics();
        if (!m) return;
        const now = performance.now();
        if (fill0 === null && m.consumedFrames > 0) { fill0 = m.fillFrames; t0 = now; }
        // Drift estimate: net fill change per wall-clock minute since steady state.
        const driftFpm = (fill0 !== null && now > t0)
            ? ((m.fillFrames - fill0) / ((now - t0) / 60000)).toFixed(1) : 'n/a';
        const rate = last ? Math.round((m.consumedFrames - last.consumedFrames) / ((now - lastT) / 1000)) : 0;
        console.log(`[bus-spike] fill=${m.fillFrames}f (${(m.fillFrames / ctx.sampleRate * 1000).toFixed(1)}ms)`
            + ` drift=${driftFpm}f/min consume=${rate}/s underflow=${m.underflowCount}`
            + ` overflow=${m.overflowCount} sent=${sentChunks} ctx=${ctx.state}`);
        last = m; lastT = now;
    }, 5000);

    window.__busSpike = {
        ctx, tap,
        stop: async () => {
            clearInterval(timer);
            try { osc.stop(); } catch (_) {}
            await ctx.close().catch(() => {});
            await api.setRendererBus(false, 0);
            URL.revokeObjectURL(modUrl);
            console.log('[bus-spike] stopped');
        },
    };
})();
