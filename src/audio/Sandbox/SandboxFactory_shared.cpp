// Sandbox factory — platform-neutral routing policy.
//
// Decides whether a plugin loads through the out-of-process sandbox and, if so,
// constructs a SandboxedProcessor. Only resolveSandboxExe() (locating the
// slopsmith-vst-host binary next to the addon) is platform-specific — it lives
// in SandboxFactory_{win,posix}.cpp.

#include "SandboxedProcessor.h"
#include "../VSTTrace.h"

#include <juce_core/juce_core.h>
#include <cmath>      // std::isfinite, std::lround
#include <limits>     // std::numeric_limits
#include <mutex>      // guards the runtime crash blocklist

namespace slopsmith::sandbox {

namespace {

// Historical pre-seed of plugins known to fail in-process. With the
// sandbox-by-default policy in shouldSandbox() below, every VST3 routes to the
// sandbox regardless of this list, so it no longer determines routing on its
// own. It survives as (a) documentation of *why* each plugin originally needed
// the sandbox, (b) diagnostic tagging in shouldSandbox's VST_TRACE output, and
// (c) forward-looking infrastructure for a future per-plugin opt-out.
const juce::StringArray kDefaultNeedsSandboxFilenames = {
    "Guitar Rig",
    "Graphene",
    "TONEX",
    "AmpliTube",
};

// Runtime crash blocklist: full plugin paths that crashed the app on a previous
// run, supplied by the renderer's VST crash guard via setCrashedPlugins().
std::mutex g_crashedPluginsMutex;
juce::StringArray g_crashedPlugins;

} // anonymous

// Routing policy: by default VST3 plugins now load IN-PROCESS for playback (see
// the rationale on the default return at the bottom of this function); only
// previously-crashed or pre-seeded plugins are forced through the out-of-process
// sandbox. Non-VST3 processors (NAM, IR) always stay in-process.
//
// In-process faults are made non-fatal by the guard in SignalChain.cpp (SEH on
// Windows, a siglongjmp signal guard on POSIX): a faulting plugin is blocklisted
// so its next load routes here to the sandbox. The sandbox child also provides
// an OS-main-thread / STA-COM host that a few plugins assume and that Electron's
// background JUCE thread does not — another reason a misbehaving plugin can be
// pinned back to it via the blocklist.
bool shouldSandbox(const juce::PluginDescription& desc)
{
    const auto path = juce::File(desc.fileOrIdentifier);

    // VST3 only: non-VST3 processors (NAM models, IRs) keep loading in-process.
    if (!path.getFileName().endsWithIgnoreCase(".vst3"))
        return false;

    // Runtime crash blocklist: a plugin that previously faulted in-process is
    // forced back to the out-of-process sandbox on every subsequent load.
    {
        const std::lock_guard<std::mutex> lock(g_crashedPluginsMutex);
        const auto canonical = path.getFullPathName();
        if (g_crashedPlugins.contains(canonical, /*ignoreCase*/ true))
        {
            VST_TRACE("shouldSandbox: %s — on the runtime crash blocklist",
                      desc.fileOrIdentifier.toRawUTF8());
            return true;
        }
    }

    // Pre-seed match: plugins known to need isolation are forced to the sandbox.
    const auto basename = path.getFileNameWithoutExtension();
    for (auto& needle : kDefaultNeedsSandboxFilenames)
    {
        if (basename.startsWithIgnoreCase(needle))
        {
            VST_TRACE("shouldSandbox: %s — filename starts with '%s'",
                      desc.fileOrIdentifier.toRawUTF8(), needle.toRawUTF8());
            return true;
        }
    }

    // Default: load in-process for PLAYBACK. A plugin reaches a chain only after
    // it scanned cleanly (the sandbox's real job is crash-isolating the SCAN of
    // unknown plugins), so the common case is known-good and the out-of-process
    // IPC (N serial round-trips/block, memcpy, poll waits) is pure overhead and
    // latency. Anything that DOES fault is caught by the SignalChain fault guard
    // (SEH on Windows, a siglongjmp signal guard on POSIX) and added to the
    // runtime crash blocklist (or the launch sentinel) above, so it falls back to
    // the sandbox on its next load. Net: known-good gear runs at native cost;
    // only the genuinely crash-prone keeps paying for isolation.
    VST_TRACE("shouldSandbox: %s — default policy: in-process (scanned/known-good)",
              desc.fileOrIdentifier.toRawUTF8());
    return false;
}

std::unique_ptr<juce::AudioProcessor> tryLoadSandboxed(
    const juce::PluginDescription& desc,
    double sampleRate, int blockSize,
    juce::String& errorOut)
{
    if (!shouldSandbox(desc))
        return nullptr;

    auto exe = resolveSandboxExe();
    if (!exe.existsAsFile())
    {
        errorOut = "slopsmith-vst-host not found";
        return nullptr;
    }

    // Validate sampleRate before narrowing to uint32_t — `(uint32_t)NaN` is UB
    // and silently accepting 0 / negative / overflow makes a bad caller surface
    // as a late sandbox-spawn failure instead of a clear errorOut here.
    if (! std::isfinite(sampleRate) || sampleRate <= 0.0
        || sampleRate > (double)(std::numeric_limits<uint32_t>::max)())
    {
        errorOut = "invalid sampleRate: " + juce::String(sampleRate);
        return nullptr;
    }

    SandboxedProcessor::SpawnConfig cfg;
    cfg.pluginPath = desc.fileOrIdentifier;
    cfg.pluginName = desc.name.isNotEmpty() ? desc.name : "plugin";
    cfg.sandboxExePath = exe.getFullPathName();
    cfg.audio.sampleRate = (uint32_t)std::lround(sampleRate);
    // Clamp to the protocol cap: vst-host's kPrepare rejects blockSize
    // > kAudioMaxBlockSamples, so spawning a larger shm layout would later fail
    // the prepare round-trip rather than silently misbehave.
    cfg.audio.maxBlockSamples = (uint32_t)juce::jlimit(
        64, (int)kAudioMaxBlockSamples, blockSize);
    cfg.audio.maxChannels = 2;
    cfg.audio.maxBlocks = kAudioMaxBlocks;

    return SandboxedProcessor::spawn(cfg, errorOut);
}

void addCrashedPlugin(const juce::String& pluginPath)
{
    if (pluginPath.isEmpty()) return;
    const auto canonical = juce::File(pluginPath).getFullPathName();
    const std::lock_guard<std::mutex> lock(g_crashedPluginsMutex);
    if (! g_crashedPlugins.contains(canonical, /*ignoreCase*/ true))
    {
        g_crashedPlugins.add(canonical);
        VST_TRACE("addCrashedPlugin: %s appended to runtime crash blocklist",
                  canonical.toRawUTF8());
    }
}

void setCrashedPlugins(const juce::StringArray& pluginPaths)
{
    const std::lock_guard<std::mutex> lock(g_crashedPluginsMutex);
    g_crashedPlugins.clearQuick();
    for (const auto& p : pluginPaths)
        g_crashedPlugins.add(p.isNotEmpty() ? juce::File(p).getFullPathName() : p);
    VST_TRACE("setCrashedPlugins: %d plugin(s) on the runtime crash blocklist",
              g_crashedPlugins.size());
}

} // namespace slopsmith::sandbox
