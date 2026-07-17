# Out-of-process VST helpers.
#
#   all platforms — slopsmith-vst-host(.exe): sandboxed VST3 host + --scan-plugin
#   macOS (also)  — slopsmith-vst-scan: VST3-only --scan-plugin probe, still the
#                   scan host VSTHost.cpp resolves on macOS (kept until the scan
#                   path migrates to slopsmith-vst-host)
#
# Binaries are statically linked with JUCE so they can sit next to the addon.
# Built on all three desktop platforms now that the POSIX sandbox runtime
# (Slice 2) gives macOS/Linux a real slopsmith-vst-host child.

# Shared JUCE wiring for both helpers: include path, link libs, compile defs,
# and the per-config output dir so multi-config generators (VS / Xcode) don't
# append a second $<CONFIG> suffix.
function(slopsmith_configure_vst_helper target app_name)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../audio"
    )

    target_link_libraries(${target} PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
    )

    target_compile_definitions(${target} PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_LV2=0
        # AU off: slopsmith_audio.node also omits JUCE_PLUGINHOST_AU (see
        # src/audio/CMakeLists.txt). On macOS, AU hosting from the Electron
        # native-addon thread crashes inside AppKit/JUCE MessageManager.
        JUCE_PLUGINHOST_AU=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_STANDALONE_APPLICATION=1
        JUCE_REPORT_APP_USAGE=0
        JUCE_APPLICATION_NAME_STRING="${app_name}"
        JUCE_APPLICATION_VERSION_STRING="0.1.0"
    )

    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY                "${CMAKE_BINARY_DIR}/Release"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${CMAKE_BINARY_DIR}/Release"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${CMAKE_BINARY_DIR}/Release"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/Release"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${CMAKE_BINARY_DIR}/Release"
        OUTPUT_NAME "${target}"
    )
endfunction()

# slopsmith-vst-host on every platform. The IPC channel backends are
# platform-split (shared + win/posix); main.cpp builds as WinMain on Windows
# and main() on POSIX (same file, #if-gated entry).
set(VST_HOST_SOURCES
    main.cpp
    ../audio/VSTHost.cpp
    ../audio/Sandbox/Protocol.cpp
    ../audio/Sandbox/ControlChannel_shared.cpp
    ../audio/Sandbox/AudioChannel_shared.cpp
)
if(WIN32)
    list(APPEND VST_HOST_SOURCES
        ../audio/Sandbox/ControlChannel_win.cpp
        ../audio/Sandbox/AudioChannel_win.cpp)
    add_executable(slopsmith-vst-host WIN32 ${VST_HOST_SOURCES})
else()
    list(APPEND VST_HOST_SOURCES
        ../audio/Sandbox/ControlChannel_posix.cpp
        ../audio/Sandbox/AudioChannel_posix.cpp)
    add_executable(slopsmith-vst-host ${VST_HOST_SOURCES})
endif()
slopsmith_configure_vst_helper(slopsmith-vst-host SlopsmithVstHost)

if(APPLE)
    # CoreAudio/MIDI frameworks for JUCE audio + plugin hosting on macOS.
    target_link_libraries(slopsmith-vst-host PRIVATE
        "-framework CoreAudio" "-framework CoreMIDI" "-framework AudioUnit"
        "-framework AudioToolbox" "-framework CoreAudioKit"
        "-framework CoreFoundation" "-framework Accelerate")
endif()

if(UNIX AND NOT APPLE)
    # Linux: main.cpp calls XInitThreads/XSetErrorHandler directly to install a
    # non-fatal X error handler so a benign plugin-editor X protocol error can't
    # exit() this child (JUCE only auto-installs those handlers for standalone
    # JUCEApplications — see installLinuxX11Safety in main.cpp). JUCE dlopen()s
    # libX11 for its own use; our direct calls need it at link time.
    find_package(X11 REQUIRED)
    target_link_libraries(slopsmith-vst-host PRIVATE ${X11_LIBRARIES})
    target_include_directories(slopsmith-vst-host PRIVATE ${X11_INCLUDE_DIR})
endif()

if(WIN32)
    if(MSVC)
        # /EHa for SEH-aware catch in VSTHost.cpp (matches src/audio/CMakeLists.txt).
        target_compile_options(slopsmith-vst-host PRIVATE /EHa)
        # Emit a PDB for Release builds so sandbox-child crash dumps can be
        # symbolised — see the matching block in src/audio/CMakeLists.txt for the
        # /Zi + /DEBUG:FULL + /OPT rationale.
        target_compile_options(slopsmith-vst-host PRIVATE $<$<CONFIG:Release>:/Zi>)
        # One generator expression per flag — see src/audio/CMakeLists.txt.
        target_link_options(slopsmith-vst-host PRIVATE
            $<$<CONFIG:Release>:/DEBUG:FULL>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>)
        # `add_executable(... WIN32 ...)` already implies /SUBSYSTEM:WINDOWS and
        # the matching entry point (WinMainCRTStartup, or wWinMainCRTStartup if
        # the toolchain ever picks up _UNICODE). No need to pin them explicitly.
    endif()
endif()

if(APPLE)
    add_executable(slopsmith-vst-scan
        VSTScan.cpp
        ../audio/VSTHost.cpp
    )
    slopsmith_configure_vst_helper(slopsmith-vst-scan SlopsmithVstScan)

    target_link_libraries(slopsmith-vst-scan PRIVATE
        "-framework CoreAudio"
        "-framework CoreMIDI"
        "-framework AudioUnit"
        "-framework AudioToolbox"
        "-framework CoreAudioKit"
        "-framework CoreFoundation"
        "-framework Accelerate"
    )
endif()
