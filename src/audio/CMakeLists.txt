# Slopsmith Audio Engine — Node.js native addon built with JUCE
# Produces slopsmith_audio.node loadable via require()

set(AUDIO_SOURCES
    NodeAddon.cpp
    NoiseGate.cpp
    TonePolish.cpp
    AudioEngine.cpp
    engine/StreamSink.cpp
    engine/BackingPlayer.cpp
    engine/DeviceSetup.cpp
    engine/SourcePool.cpp
    engine/ExtraInputs.cpp
    addon/AddonContext.cpp
    addon/ChainOps.cpp
    addon/EditorWindows.cpp
    addon/DeviceBindings.cpp
    addon/ControlBindings.cpp
    addon/DetectionBindings.cpp
    addon/ChainBindings.cpp
    addon/BackingBindings.cpp
    SourceChain.cpp
    SignalChain.cpp
    VSTHost.cpp
    NAMProcessor.cpp
    IRLoader.cpp
    PitchDetector.cpp
    ChordScorer.cpp
    MlNoteDetector.cpp
    OnsetDetector.cpp
    NoteVerifier.cpp
    Sandbox/Protocol.cpp
)

# Plugin sandbox — out-of-process VST3 host for plugins that don't survive
# in-process loading (notably Qt5-using plugins from Native Instruments;
# see docs/VST-SANDBOX-DIAG.md for the diagnosis).
#
# Cross-platform (constitution VIII): the IPC layer is a platform-neutral core
# (*_shared.cpp) plus per-OS backends (*_win.cpp / *_posix.cpp). All three
# desktop platforms now route VST3 plugins through the out-of-process sandbox.
# The slopsmith-vst-host child that the factory spawns is built by the same
# cmake-js invocation (src/vst-host) and lands next to slopsmith_audio.node, so
# resolveSandboxExe() finds it; if it is ever missing the factory returns
# nullptr and the caller falls back to in-process loading.
set(SANDBOX_SOURCES
    Sandbox/SandboxedProcessor.cpp
    Sandbox/CrashAttribution.cpp
    Sandbox/ControlChannel_shared.cpp
    Sandbox/AudioChannel_shared.cpp
    Sandbox/SandboxFactory_shared.cpp
)
if(WIN32)
    list(APPEND SANDBOX_SOURCES
        Sandbox/ControlChannel_win.cpp
        Sandbox/AudioChannel_win.cpp
        Sandbox/SubprocessHandle_win.cpp
        Sandbox/SandboxFactory_win.cpp
    )
else()
    list(APPEND SANDBOX_SOURCES
        Sandbox/ControlChannel_posix.cpp
        Sandbox/AudioChannel_posix.cpp
        Sandbox/SubprocessHandle_posix.cpp
        Sandbox/SandboxFactory_posix.cpp
    )
endif()
list(APPEND AUDIO_SOURCES ${SANDBOX_SOURCES})

# Build as shared library (.node)
add_library(slopsmith_audio SHARED ${AUDIO_SOURCES} ${CMAKE_JS_SRC})

# Output as .node file
set_target_properties(slopsmith_audio PROPERTIES
    PREFIX ""
    SUFFIX ".node"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release"
)

# Node.js / N-API headers
if(DEFINED CMAKE_JS_INC)
    target_include_directories(slopsmith_audio PRIVATE ${CMAKE_JS_INC})
endif()

# node-addon-api headers (installed via npm)
execute_process(
    COMMAND node -p "require('node-addon-api').include"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE NAPI_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REPLACE "\"" "" NAPI_INCLUDE_DIR "${NAPI_INCLUDE_DIR}")
target_include_directories(slopsmith_audio PRIVATE ${NAPI_INCLUDE_DIR})

# N-API version
target_compile_definitions(slopsmith_audio PRIVATE NAPI_VERSION=9 NAPI_DISABLE_CPP_EXCEPTIONS)

# Marks this translation unit set as the audio addon (vs. slopsmith-vst-host,
# which also compiles VSTHost.cpp). Gates the out-of-process VST scan path so
# it's only built into the addon — the host exe never calls scanDirectories.
target_compile_definitions(slopsmith_audio PRIVATE SLOPSMITH_AUDIO_ADDON=1)

# JUCE modules — headless (no GUI)
target_link_libraries(slopsmith_audio PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_core
    juce::juce_dsp
    juce::juce_events
)

# JUCE compile definitions
target_compile_definitions(slopsmith_audio PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_APPLICATION_NAME_STRING="SlopsmithAudio"
    JUCE_STANDALONE_APPLICATION=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_LOG_ASSERTIONS=0
    JUCE_DISABLE_ASSERTIONS=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_FLAC=1
    # Backing tracks are commonly .mp3 (song backing-track audio). Without
    # this, registerBasicFormats() omits MP3 and loadBackingTrack() rejects
    # every mp3, forcing the song onto the HTML5 path. JUCE's built-in MP3
    # decoder is patent-unencumbered and ships with the framework.
    JUCE_USE_MP3AUDIOFORMAT=1
)

# Platform-specific
if(APPLE)
    # NOTE: JUCE_PLUGINHOST_AU=1 requires linking CoreAudioKit/AppKit for
    # AUGenericView, but doing so causes JUCE's MessageManager to crash
    # when run from a Node.js native-addon thread (AppKit's NSRunLoop
    # expects the main thread). Disabling AU hosting on macOS until the
    # NodeAddon message-loop integration is redesigned. VST3 and NAM still
    # work; users can load VST3 plugins instead of AU.
    target_link_libraries(slopsmith_audio PRIVATE
        "-framework CoreAudio"
        "-framework CoreMIDI"
        "-framework AudioUnit"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework Accelerate"
    )
elseif(WIN32)
    target_compile_definitions(slopsmith_audio PRIVATE
        JUCE_ASIO=1
    )
    # Enable /EHa so catch(...) also catches SEH exceptions (ASIO driver crashes)
    if(MSVC)
        target_compile_options(slopsmith_audio PRIVATE /EHa)
        # Emit a PDB for Release builds so field crash dumps (WER minidumps
        # from testers) can be symbolised — without one, our own frames in a
        # dump are just raw addresses. /Zi adds debug info to the objects;
        # /DEBUG:FULL makes the linker write a standalone, portable PDB but
        # also turns /OPT:REF + /OPT:ICF off by default, so re-enable them —
        # the .node stays a fully optimised release build that merely also
        # ships a matching PDB. The PDB is not bundled into the app: the
        # electron-builder `files` config picks up *.node / onnxruntime* by
        # name, not *.pdb.
        target_compile_options(slopsmith_audio PRIVATE $<$<CONFIG:Release>:/Zi>)
        # One generator expression per flag: a single $<...:A B C> would be
        # split on its internal spaces into separate — and malformed — CMake
        # arguments.
        target_link_options(slopsmith_audio PRIVATE
            $<$<CONFIG:Release>:/DEBUG:FULL>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>)
    endif()
elseif(UNIX)
    # LV2 is Linux-only — on Windows, lilv probes every .vst3 bundle for a
    # manifest.ttl during scan and crashes the audio addon.
    target_compile_definitions(slopsmith_audio PRIVATE
        JUCE_PLUGINHOST_LV2=1
        JUCE_JACK=1
        JUCE_JACK_CLIENT_NAME="Slopsmith"
        JUCE_ALSA=1
    )
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    target_link_libraries(slopsmith_audio PRIVATE ${ALSA_LIBRARIES})
    target_include_directories(slopsmith_audio PRIVATE ${ALSA_INCLUDE_DIRS})

    # JACK is loaded dynamically by JUCE
    find_library(JACK_LIB jack)
    if(JACK_LIB)
        target_link_libraries(slopsmith_audio PRIVATE ${JACK_LIB})
    endif()
endif()

# cmake-js library
if(DEFINED CMAKE_JS_LIB)
    target_link_libraries(slopsmith_audio PRIVATE ${CMAKE_JS_LIB})
endif()

# NAM support
if(NAM_AVAILABLE)
    # Include NAM core sources directly. GLOB_RECURSE so the A2 architecture
    # sources under NAM/wavenet/ (added in NeuralAmpModelerCore v0.5.x) are
    # picked up, not just the flat NAM/*.cpp files.
    file(GLOB_RECURSE NAM_SOURCES "${NAM_DIR}/NAM/*.cpp")
    target_sources(slopsmith_audio PRIVATE ${NAM_SOURCES})
    target_include_directories(slopsmith_audio PRIVATE
        "${NAM_DIR}"
        "${NAM_DIR}/Dependencies/eigen"
        "${NAM_DIR}/Dependencies/nlohmann"
        "${NAM_DIR}/Dependencies/AudioDSPTools"
    )
    # SLOPSMITH_NAM_SUPPORT gates our NAMProcessor; NAM_ENABLE_A2_FAST enables
    # the hand-optimized A2 fast-path WaveNet (a2_fast.cpp / model.cpp are fully
    # #if-guarded on it). Upstream defaults this ON; we compile NAM sources
    # directly (no add_subdirectory) so we must set the define ourselves.
    target_compile_definitions(slopsmith_audio PRIVATE
        SLOPSMITH_NAM_SUPPORT=1
        NAM_ENABLE_A2_FAST
    )

    if(RTNEURAL_AVAILABLE)
        target_link_libraries(slopsmith_audio PRIVATE RTNeural)
    endif()
else()
    target_compile_definitions(slopsmith_audio PRIVATE SLOPSMITH_NAM_SUPPORT=0)
endif()

# ONNX Runtime — MlNoteDetector (Basic Pitch). When unavailable, the addon
# still builds and MlNoteDetector is compiled as an inert stub; the engine
# falls back to the YIN PitchDetector / ChordScorer (Constitution VII).
if(ONNXRUNTIME_AVAILABLE)
    target_include_directories(slopsmith_audio PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
    target_link_libraries(slopsmith_audio PRIVATE "${ONNXRUNTIME_IMPORT_LIB}")
    target_compile_definitions(slopsmith_audio PRIVATE SLOPSMITH_ONNX_SUPPORT=1)

    # The ONNX Runtime shared lib must load next to slopsmith_audio.node.
    # Linux: rpath $ORIGIN; macOS: @loader_path; Windows: DLL is searched in
    # the module's own directory by default.
    if(APPLE)
        set_target_properties(slopsmith_audio PROPERTIES
            BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
    elseif(UNIX)
        set_target_properties(slopsmith_audio PROPERTIES
            BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
    endif()

    # Stage the runtime lib beside the .node so dev runs and electron-builder
    # packaging (asarUnpack of build/Release/*) both find it.
    add_custom_command(TARGET slopsmith_audio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_RUNTIME_LIB}"
                "$<TARGET_FILE_DIR:slopsmith_audio>/"
        COMMENT "Staging ONNX Runtime shared library next to slopsmith_audio.node")

    # Also stage the shared-provider stub that ONNX Runtime dlopen()s next to
    # the main runtime. Conditional: it ships in the standard releases, but the
    # cmake probe leaves ONNXRUNTIME_PROVIDERS_LIB empty if a layout lacks it.
    if(ONNXRUNTIME_PROVIDERS_LIB)
        add_custom_command(TARGET slopsmith_audio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${ONNXRUNTIME_PROVIDERS_LIB}"
                    "$<TARGET_FILE_DIR:slopsmith_audio>/"
            COMMENT "Staging ONNX Runtime providers_shared library next to slopsmith_audio.node")
    endif()

    # macOS only: the prebuilt onnxruntime dylib bakes its absolute build-time
    # extraction path into LC_ID_DYLIB, which the linker then copies into the
    # addon as an absolute LC_LOAD_DYLIB. The @loader_path rpath set above is
    # never consulted for an absolute load path, so the addon loads onnxruntime
    # only on the build machine — everywhere else MlNoteDetector can't init and
    # the engine silently falls back to YIN ("ML detection: OFF", slopsmith#818).
    # Rewrite the install names to be @rpath-relative so the co-located runtime
    # resolves on every Mac. Runs after staging so it rewrites the staged copy.
    if(APPLE)
        get_filename_component(_ort_runtime_name "${ONNXRUNTIME_RUNTIME_LIB}" NAME)
        add_custom_command(TARGET slopsmith_audio POST_BUILD
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/fix-onnxruntime-install-names.sh"
                    "$<TARGET_FILE:slopsmith_audio>"
                    "${_ort_runtime_name}"
            COMMENT "Normalising ONNX Runtime install names to @rpath (slopsmith#818)")
    endif()
else()
    target_compile_definitions(slopsmith_audio PRIVATE SLOPSMITH_ONNX_SUPPORT=0)
endif()

# Signalsmith Stretch: pitch-preserving time-stretch for the backing track.
# signalsmith-stretch.h includes "signalsmith-linear/stft.h",
# so the linear repo's parent must also be in the include path.
set(SS_STRETCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/signalsmith-stretch")
set(SS_LINEAR_PARENT "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_include_directories(slopsmith_audio PRIVATE
    "${SS_STRETCH_DIR}"
    "${SS_LINEAR_PARENT}")

# Include directory for our headers
target_include_directories(slopsmith_audio PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
