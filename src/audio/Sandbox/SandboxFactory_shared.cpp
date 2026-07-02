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

// Pre-seed of plugins known to fail when hosted in-process. Under the current
// in-process-by-default policy (see shouldSandbox() below) this list DOES drive
// routing: a filename matched here is forced to the out-of-process sandbox
// instead of loading in-process. Matched against the plugin's basename
// (case-insensitive prefix).
const juce::StringArray kDefaultNeedsSandboxFilenames = {
    "Guitar Rig",
    "Graphene",
    "TONEX",
    "AmpliTube",
};

// Vendor/path fragments that force the sandbox regardless of filename — matched
// (case-insensitive substring) against the full plugin path. Use this for a
// whole vendor whose plugins share an install folder rather than enumerating
// every product. Prefer the most specific reliable fragment (the vendor's
// install-folder name, not a bare brand word): there is NO in-process fallback
// here — loadVstSandboxAware hard-fails the load if a force-sandboxed plugin
// can't spawn the sandbox child — so a false-positive match on an unrelated
// path would turn a fine in-process plugin into a load failure on a machine
// with a broken sandbox host.
//
// PolyChrome DSP (McRocklin Suite, Graphene, …) creates a top-level window on
// the host message thread during in-process init. On Electron's BACKGROUND JUCE
// message thread that window's WndProc ends up in non-executable memory, so when
// Windows broadcasts WM_ACTIVATEAPP the OS message pump executes it → an
// execute-DEP access violation (0xC0000005) that kills the app. That crash
// arrives via USER32→WndProc with NO host frame on the stack, so the SignalChain
// fault guard cannot catch it and the runtime blocklist never gets to record it.
// The sandbox child hosts the plugin on a real top-level message thread, which
// both isolates the fault and is the environment the plugin actually needs.
// (Diagnosed from crash dump a06f48e1: Rax==Rip==McRocklin Suite.vst3 WndProc,
// caller USER32+0xEF5C, msg=WM_ACTIVATEAPP. The vendor ships to
// Common Files/VST3/PolyChrome DSP/, so that folder name is the reliable match.)
const juce::StringArray kDefaultNeedsSandboxPathFragments = {
    "PolyChrome DSP",
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

    // Canonical path, computed once and reused by the blocklist + vendor checks.
    const auto fullPath = path.getFullPathName();

    // Runtime crash blocklist: a plugin that previously faulted in-process is
    // forced back to the out-of-process sandbox on every subsequent load.
    {
        const std::lock_guard<std::mutex> lock(g_crashedPluginsMutex);
        if (g_crashedPlugins.contains(fullPath, /*ignoreCase*/ true))
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

    // Vendor/path pre-seed: force whole vendors known to fail in-process (e.g.
    // PolyChrome DSP — see kDefaultNeedsSandboxPathFragments) to the sandbox,
    // even if their individual filenames aren't enumerated above.
    for (auto& fragment : kDefaultNeedsSandboxPathFragments)
    {
        if (fullPath.containsIgnoreCase(fragment))
        {
            VST_TRACE("shouldSandbox: %s — path contains '%s' (vendor needs sandbox)",
                      desc.fileOrIdentifier.toRawUTF8(), fragment.toRawUTF8());
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

bool isCrashedPlugin(const juce::String& pluginPath)
{
    if (pluginPath.isEmpty()) return false;
    const auto canonical = juce::File(pluginPath).getFullPathName();
    const std::lock_guard<std::mutex> lock(g_crashedPluginsMutex);
    return g_crashedPlugins.contains(canonical, /*ignoreCase*/ true);
}

void removeCrashedPlugin(const juce::String& pluginPath)
{
    if (pluginPath.isEmpty()) return;
    const auto canonical = juce::File(pluginPath).getFullPathName();
    const std::lock_guard<std::mutex> lock(g_crashedPluginsMutex);
    // Case-insensitive match, mirroring addCrashedPlugin/shouldSandbox's
    // contains(..., ignoreCase=true). Iterate backwards to remove safely.
    bool removed = false;
    for (int i = g_crashedPlugins.size(); --i >= 0;)
    {
        if (g_crashedPlugins[i].equalsIgnoreCase(canonical))
        {
            g_crashedPlugins.remove(i);
            removed = true;
        }
    }
    if (removed)
        VST_TRACE("removeCrashedPlugin: %s removed from runtime crash blocklist",
                  canonical.toRawUTF8());
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
