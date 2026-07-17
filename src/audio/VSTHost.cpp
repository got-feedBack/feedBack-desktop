#include "VSTHost.h"
#include "VSTTrace.h"
#include "addon/PluginModulePin.h"

// The out-of-process scan path is compiled only into the audio addon
// (SLOPSMITH_AUDIO_ADDON, set in src/audio/CMakeLists.txt). slopsmith-vst-host
// also links VSTHost.cpp but must NOT pull in SandboxFactory — and it never
// calls scanDirectories anyway (it runs the --scan-plugin one-shot instead).
#if JUCE_WINDOWS && defined(SLOPSMITH_AUDIO_ADDON)
 #include "Sandbox/SandboxedProcessor.h"
#endif

#if defined(SLOPSMITH_AUDIO_ADDON) && (JUCE_WINDOWS || JUCE_MAC)
namespace {

#if JUCE_MAC
#include <dlfcn.h>

// Anchor in this TU so dladdr resolves slopsmith_audio.node, not Electron.
static int macAudioAddonDlAddrAnchor() { return 0; }

// Directory containing slopsmith_audio.node (not Electron's executable).
static juce::File resolveMacAddonDirectory()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&macAudioAddonDlAddrAnchor),
               &info) != 0
        && info.dli_fname != nullptr
        && info.dli_fname[0] != '\0')
    {
        const juce::File addonFile(info.dli_fname);
        if (addonFile.existsAsFile())
            return addonFile.getParentDirectory();
    }
    return {};
}

// macOS: slopsmith-vst-scan (built by src/vst-host/CMakeLists.txt).
static juce::File resolveMacScanHostExecutable()
{
    if (const char* env = std::getenv("SLOPSMITH_VST_SCAN_HOST"))
    {
        const juce::File fromEnv(env);
        if (fromEnv.existsAsFile())
            return fromEnv;
    }

    juce::Array<juce::File> candidates;

    // Packaged + dev: helper sits next to slopsmith_audio.node in
    // build/Release/ or app.asar.unpacked/build/Release/.
    const auto addonDir = resolveMacAddonDirectory();
    if (addonDir.isDirectory())
        candidates.add(addonDir.getChildFile("slopsmith-vst-scan"));

    // npm run dev when cwd is slopsmith-desktop/
    candidates.add(juce::File::getCurrentWorkingDirectory()
                         .getChildFile("build/Release/slopsmith-vst-scan"));
    // Source-tree anchor when cwd differs
    candidates.add(juce::File(__FILE__).getParentDirectory()
                         .getParentDirectory()
                         .getParentDirectory()
                         .getChildFile("build/Release/slopsmith-vst-scan"));

    for (const auto& c : candidates)
        if (c.existsAsFile())
            return c;

    return {};
}
#endif

// Probe one plugin file in a child scan host (slopsmith-vst-host.exe /
// slopsmith-vst-scan) so a plugin that
// crashes / aborts / hangs during init can't take down the host process.
// Returns the descriptor XML on success; sets `reason` and returns empty on
// failure (spawn failure, timeout, non-zero exit, or no output).
juce::String scanPluginOutOfProcess(const juce::File& hostExe,
                                    const juce::String& pluginPath,
                                    int timeoutMs,
                                    juce::String& reason)
{
    const juce::File outFile = juce::File::createTempFile(".scan.xml");

    juce::ChildProcess proc;
    const juce::StringArray args {
        hostExe.getFullPathName(),
        "--scan-plugin", pluginPath,
        "--scan-out",    outFile.getFullPathName(),
    };
    if (! proc.start(args, 0))
    {
        reason = "failed to spawn scan host";
        outFile.deleteFile();
        return {};
    }
    if (! proc.waitForProcessToFinish(timeoutMs))
    {
        // A plugin that hangs during init (license-wait deadlock, modal
        // dialog) never returns — kill the child and move on.
        proc.kill();
        reason = "scan timed out after " + juce::String(timeoutMs) + " ms";
        outFile.deleteFile();
        return {};
    }
    const auto exitCode = proc.getExitCode();
    if (exitCode != 0)
    {
        reason = "scan host exited with code " + juce::String((int) exitCode);
        outFile.deleteFile();
        return {};
    }
    const juce::String xml = outFile.loadFileAsString();
    outFile.deleteFile();
    if (xml.isEmpty())
    {
        reason = "scan host produced no output";
        return {};
    }
    return xml;
}

static juce::File resolveOutOfProcessScanHost()
{
#if JUCE_WINDOWS
    return slopsmith::sandbox::resolveSandboxExe();
#elif JUCE_MAC
    return resolveMacScanHostExecutable();
#else
    return {};
#endif
}

} // anonymous
#endif

VSTHost::VSTHost()
{
    formatManager.addFormat(std::make_unique<juce::VST3PluginFormat>());

#if JUCE_PLUGINHOST_AU
    formatManager.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
#endif

#if JUCE_PLUGINHOST_LV2
    formatManager.addFormat(std::make_unique<juce::LV2PluginFormat>());
#endif
}

VSTHost::~VSTHost()
{
    cancelScan();
}

// ── Scanning ──────────────────────────────────────────────────────────────────

juce::StringArray VSTHost::getDefaultScanDirectories()
{
    juce::StringArray dirs;

#if JUCE_LINUX
    dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
             .getChildFile(".vst3").getFullPathName());
    dirs.add("/usr/lib/vst3");
    dirs.add("/usr/local/lib/vst3");
    // LV2
    dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
             .getChildFile(".lv2").getFullPathName());
   #if JUCE_64BIT
    if (juce::File ("/usr/lib64/lv2").exists())
    {
        dirs.add("/usr/local/lib64/lv2");
        dirs.add("/usr/lib64/lv2");
    }
    else
   #endif
    {
        dirs.add("/usr/lib/lv2");
        dirs.add("/usr/local/lib/lv2");
    }
#elif JUCE_MAC
    dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
             .getChildFile("Library/Audio/Plug-Ins/VST3").getFullPathName());
    dirs.add("/Library/Audio/Plug-Ins/VST3");
#if JUCE_PLUGINHOST_AU
    dirs.add(juce::File::getSpecialLocation(juce::File::userHomeDirectory)
             .getChildFile("Library/Audio/Plug-Ins/Components").getFullPathName());
    dirs.add("/Library/Audio/Plug-Ins/Components");
#endif
#elif JUCE_WINDOWS
    dirs.add("C:\\Program Files\\Common Files\\VST3");
    dirs.add("C:\\Program Files (x86)\\Common Files\\VST3");
    auto localAppData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
    dirs.add(localAppData.getChildFile("VST3").getFullPathName());
#endif

    return dirs;
}

void VSTHost::scanDefaultDirectories(ScanProgressCallback callback)
{
    scanDirectories(getDefaultScanDirectories(), std::move(callback));
}

namespace {

bool isFormatSupported(const juce::AudioPluginFormatManager& fm,
                       const juce::PluginDescription& desc)
{
    for (auto* format : fm.getFormats())
        if (format->getName() == desc.pluginFormatName)
            return true;
    return false;
}

} // namespace

void VSTHost::scanDirectories(const juce::StringArray& directories, ScanProgressCallback callback)
{
    if (scanning.load()) return;

    scanning.store(true);
    scanCancelled.store(false);

    // Collect all plugin files first
    juce::StringArray filesToScan;
    for (auto& dir : directories)
    {
        juce::File d(dir);
        if (!d.isDirectory()) continue;

        // VST3
        for (auto& f : d.findChildFiles(juce::File::findFilesAndDirectories, true, "*.vst3"))
            filesToScan.addIfNotAlreadyThere(f.getFullPathName());

        // AU (.component) — only when this binary can actually load AudioUnits.
        // The Electron addon deliberately omits JUCE_PLUGINHOST_AU (see
        // src/audio/CMakeLists.txt); scanning Components would list duplicates
        // that fail at load with "No compatible plug-in format exists".
#if JUCE_MAC && JUCE_PLUGINHOST_AU
        for (auto& f : d.findChildFiles(juce::File::findFilesAndDirectories, true, "*.component"))
            filesToScan.addIfNotAlreadyThere(f.getFullPathName());
#endif

        // LV2
#if JUCE_PLUGINHOST_LV2
        for (auto& f : d.findChildFiles(juce::File::findDirectories, true, "*.lv2"))
            filesToScan.addIfNotAlreadyThere(f.getFullPathName());
#endif
    }

    const int totalFiles = filesToScan.size();
    int scannedCount = 0;

#if defined(SLOPSMITH_AUDIO_ADDON) && (JUCE_WINDOWS || JUCE_MAC)
    // Out-of-process scan: one child per plugin file. In-process scanAndAddFile
    // inside Electron can SIGTRAP/abort on certain plugins.
    {
        // Skip helper resolution entirely when there's nothing to probe — a
        // missing helper shouldn't block an "everything was uninstalled"
        // rescan from clearing the catalog.
        juce::File hostExe;
        if (! filesToScan.isEmpty())
        {
            hostExe = resolveOutOfProcessScanHost();
            if (! hostExe.existsAsFile())
            {
                juce::Logger::writeToLog("VST scan: out-of-process scan host not found —"
                                         " aborting rescan (plugin list unchanged)");
                scanning.store(false);
                return;
            }
        }

        // Stage in a temp list and swap into knownPlugins only after a clean
        // pass. Cancel-mid-scan or every-probe-fails (broken helper env, etc.)
        // leaves the live catalog untouched instead of wiping it to empty.
        juce::KnownPluginList staged;
        bool completed = true;
        constexpr int kScanTimeoutMs = 20000;

        for (auto& file : filesToScan)
        {
            if (scanCancelled.load()) { completed = false; break; }

            juce::String reason;
            const juce::String xml = scanPluginOutOfProcess(
                hostExe, file, kScanTimeoutMs, reason);
            if (xml.isEmpty() || ! mergePluginsFromXmlInto(xml, staged))
            {
                if (reason.isEmpty())
                    reason = "scan host produced unparseable output";
                juce::Logger::writeToLog("VST scan: skipped " + file
                                         + " — " + reason);
            }

            ++scannedCount;
            const float progress = totalFiles > 0
                ? (float) scannedCount / (float) totalFiles : 1.0f;
            if (callback)
                callback(progress,
                         juce::File(file).getFileNameWithoutExtension());
        }

        // Swap only if the pass completed AND it actually produced results
        // (or there were no plugins to scan — then an empty catalog is correct).
        if (completed && (totalFiles == 0 || staged.getNumTypes() > 0))
        {
            const juce::ScopedLock sl(listLock);
            knownPlugins.clear();
            for (auto& desc : staged.getTypes())
                knownPlugins.addType(desc);
        }

        scanning.store(false);
        return;
    }
#endif

#if defined(SLOPSMITH_AUDIO_ADDON) && ! (JUCE_WINDOWS || JUCE_MAC)
    // Linux: no out-of-process scan host. Stage into a temp list and swap on
    // success so a cancelled scan doesn't wipe stale-but-still-loadable rows.
    juce::KnownPluginList linuxStaged;
    bool linuxRescanCompleted = true;
#endif

    // In-process scan (Linux addon, or non-addon builds).
    for (auto& file : filesToScan)
    {
        if (scanCancelled.load())
        {
#if defined(SLOPSMITH_AUDIO_ADDON) && ! (JUCE_WINDOWS || JUCE_MAC)
            linuxRescanCompleted = false;
#endif
            break;
        }

        juce::String pluginName = juce::File(file).getFileNameWithoutExtension();

        for (auto* format : formatManager.getFormats())
        {
            if (scanCancelled.load())
            {
#if defined(SLOPSMITH_AUDIO_ADDON) && ! (JUCE_WINDOWS || JUCE_MAC)
                linuxRescanCompleted = false;
#endif
                break;
            }

            juce::OwnedArray<juce::PluginDescription> found;
#if defined(SLOPSMITH_AUDIO_ADDON) && ! (JUCE_WINDOWS || JUCE_MAC)
            // Local list — no lock needed; swapped into knownPlugins below.
            linuxStaged.scanAndAddFile(file, true, found, *format);
#else
            {
                const juce::ScopedLock sl(listLock);
                knownPlugins.scanAndAddFile(file, true, found, *format);
            }
#endif

            for (auto* desc : found)
                pluginName = desc->name;
        }

        scannedCount++;
        float progress = totalFiles > 0 ? (float)scannedCount / (float)totalFiles : 1.0f;
        if (callback) callback(progress, pluginName);
    }

#if defined(SLOPSMITH_AUDIO_ADDON) && ! (JUCE_WINDOWS || JUCE_MAC)
    if (linuxRescanCompleted
        && (totalFiles == 0 || linuxStaged.getNumTypes() > 0))
    {
        const juce::ScopedLock sl(listLock);
        knownPlugins.clear();
        for (auto& desc : linuxStaged.getTypes())
            knownPlugins.addType(desc);
    }
#endif

    scanning.store(false);
}

juce::String VSTHost::scanPluginFileToXml(const juce::String& path)
{
    juce::XmlElement root("PLUGINS");
    for (auto* format : formatManager.getFormats())
    {
        juce::OwnedArray<juce::PluginDescription> found;
        {
            const juce::ScopedLock sl(listLock);
            knownPlugins.scanAndAddFile(path, true, found, *format);
        }
        for (auto* desc : found)
            root.addChildElement(desc->createXml().release());
    }
    // Always a parseable document — <PLUGINS/> when the file yields nothing,
    // so the parent treats "scanned, empty" as success rather than failure.
    return root.toString();
}

bool VSTHost::mergePluginsFromXmlInto(const juce::String& xml,
                                      juce::KnownPluginList& target) const
{
    const auto parsed = juce::parseXML(xml);
    if (parsed == nullptr || ! parsed->hasTagName("PLUGINS"))
        return false;

    for (auto* child : parsed->getChildIterator())
    {
        juce::PluginDescription desc;
        if (! desc.loadFromXml(*child))
            continue;

#if defined(SLOPSMITH_AUDIO_ADDON)
        // Scan helper may probe formats the addon cannot host (e.g. AU in
        // slopsmith-vst-scan). Skip them so the UI does not list unloadable dupes.
        if (! isFormatSupported(formatManager, desc))
            continue;
#endif

        target.addType(desc);
    }
    return true;
}

bool VSTHost::addPluginsFromXml(const juce::String& xml)
{
    const juce::ScopedLock sl(listLock);
    return mergePluginsFromXmlInto(xml, knownPlugins);
}

// ── Plugin Access ─────────────────────────────────────────────────────────────

juce::Array<VSTHost::PluginInfo> VSTHost::getKnownPlugins() const
{
    juce::Array<PluginInfo> result;
    const juce::ScopedLock sl(listLock);

    for (auto& desc : knownPlugins.getTypes())
    {
        PluginInfo info;
        info.name = desc.name;
        info.manufacturer = desc.manufacturerName;
        info.category = desc.category;
        info.formatName = desc.pluginFormatName;
        info.fileOrIdentifier = desc.fileOrIdentifier;
        info.uid = desc.createIdentifierString();
        info.isInstrument = desc.isInstrument;
        result.add(info);
    }

    return result;
}

std::unique_ptr<juce::AudioPluginInstance> VSTHost::loadPlugin(
    const juce::String& fileOrIdentifier,
    double sampleRate, int blockSize,
    juce::String& errorMessage)
{
    // Find matching description
    juce::PluginDescription matchedDesc;
    bool found = false;

    {
        const juce::ScopedLock sl(listLock);
        for (auto& desc : knownPlugins.getTypes())
        {
            if (desc.fileOrIdentifier == fileOrIdentifier ||
                desc.createIdentifierString() == fileOrIdentifier)
            {
                matchedDesc = desc;
                found = true;
                break;
            }
        }
    }

    if (!found)
    {
        // Try scanning the file directly if not in known list
        juce::OwnedArray<juce::PluginDescription> descs;
        for (auto* format : formatManager.getFormats())
        {
            const juce::ScopedLock sl(listLock);
            knownPlugins.scanAndAddFile(fileOrIdentifier, true, descs, *format);
        }

        if (descs.isEmpty())
        {
            errorMessage = "Plugin not found: " + fileOrIdentifier;
            return nullptr;
        }

        matchedDesc = *descs[0];
    }

    // Create instance synchronously
    juce::String error;
    VST_TRACE("VSTHost.loadPlugin: createPluginInstance BEGIN  name='%s' format='%s' file='%s' sr=%.0f bs=%d",
              matchedDesc.name.toRawUTF8(),
              matchedDesc.pluginFormatName.toRawUTF8(),
              matchedDesc.fileOrIdentifier.toRawUTF8(),
              sampleRate, blockSize);
    auto instance = formatManager.createPluginInstance(
        matchedDesc, sampleRate, blockSize, error);
    VST_TRACE("VSTHost.loadPlugin: createPluginInstance END    instance=%s error='%s'",
              instance ? "OK" : "null",
              error.toRawUTF8());

    if (!instance)
    {
        errorMessage = error.isNotEmpty() ? error : "Failed to create plugin instance";
        return nullptr;
    }

    // P0 (guide §12): never unload a plugin module mid-session. JUCE frees
    // the module when its last instance dies; a message already queued to a
    // window/timer in that module then indirect-calls into unmapped code —
    // the CFG fail-fast in the 2026-07-17 field dumps.
    slopsmith::addon::pinPluginModuleForever(
        matchedDesc.fileOrIdentifier.toRawUTF8());

    return instance;
}

void VSTHost::loadPluginAsync(
    const juce::String& fileOrIdentifier,
    double sampleRate, int blockSize,
    std::function<void(std::unique_ptr<juce::AudioPluginInstance>, juce::String)> callback)
{
    // Same matchedDesc lookup as the sync loadPlugin above. Kept inline
    // rather than factored out so the two paths can be read independently.
    juce::PluginDescription matchedDesc;
    bool found = false;
    {
        const juce::ScopedLock sl(listLock);
        for (auto& desc : knownPlugins.getTypes())
        {
            if (desc.fileOrIdentifier == fileOrIdentifier
                || desc.createIdentifierString() == fileOrIdentifier)
            {
                matchedDesc = desc;
                found = true;
                break;
            }
        }
    }

    if (!found)
    {
        juce::OwnedArray<juce::PluginDescription> descs;
        for (auto* format : formatManager.getFormats())
        {
            const juce::ScopedLock sl(listLock);
            knownPlugins.scanAndAddFile(fileOrIdentifier, true, descs, *format);
        }
        if (descs.isEmpty())
        {
            callback(nullptr, "Plugin not found: " + fileOrIdentifier);
            return;
        }
        matchedDesc = *descs[0];
    }

    VST_TRACE("VSTHost.loadPluginAsync: createPluginInstanceAsync BEGIN  "
              "name='%s' format='%s' file='%s' sr=%.0f bs=%d",
              matchedDesc.name.toRawUTF8(),
              matchedDesc.pluginFormatName.toRawUTF8(),
              matchedDesc.fileOrIdentifier.toRawUTF8(),
              sampleRate, blockSize);

    // createPluginInstanceAsync pumps the message thread while the plugin
    // initialises. The callback fires on the message thread when the load
    // completes (or fails). Move the user's callback in so a single shared
    // copy threads through both lambda hops.
    formatManager.createPluginInstanceAsync(
        matchedDesc, sampleRate, blockSize,
        [cb = std::move(callback), name = matchedDesc.name,
         fileOrId = matchedDesc.fileOrIdentifier]
        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error)
        {
            VST_TRACE("VSTHost.loadPluginAsync: createPluginInstanceAsync END    "
                      "name='%s' instance=%s error='%s'",
                      name.toRawUTF8(),
                      instance ? "OK" : "null",
                      error.toRawUTF8());

            if (!instance)
            {
                cb(nullptr,
                   error.isNotEmpty() ? error
                                      : juce::String("Failed to create plugin instance"));
                return;
            }
            // P0: pin — see the sync loadPlugin path for rationale.
            slopsmith::addon::pinPluginModuleForever(fileOrId.toRawUTF8());
            cb(std::move(instance), {});
        });
}

// ── Persistence ───────────────────────────────────────────────────────────────

void VSTHost::savePluginList(const juce::File& xmlFile)
{
    const juce::ScopedLock sl(listLock);
    if (auto xml = knownPlugins.createXml())
        xml->writeTo(xmlFile);
}

void VSTHost::loadPluginList(const juce::File& xmlFile)
{
    if (!xmlFile.existsAsFile()) return;

    if (auto xml = juce::XmlDocument::parse(xmlFile))
    {
        const juce::ScopedLock sl(listLock);
        knownPlugins.recreateFromXml(*xml);
    }
}
