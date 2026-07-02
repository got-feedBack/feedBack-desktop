# Standalone end-to-end harness for the out-of-process sandbox runtime.
# JUCE-only (no cmake-js / node-addon-api / ONNX): builds a passthrough VST3
# fixture, the real slopsmith-vst-host child, and a host-side driver that spawns
# the child, loads the plugin, and round-trips audio over the shm ring.
#
# Heavier than tests/sandbox/standalone (it pulls in juce_audio_processors +
# juce_gui_basics + a VST3), so it lives in its own bootstrap / CI job. POSIX
# only — the e2e driver uses the fd-passing host API.
#
#   cmake -S tests/sandbox/e2e -B build/e2e -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build/e2e
#   ctest --test-dir build/e2e --output-on-failure
cmake_minimum_required(VERSION 3.22)
project(slopsmith_sandbox_e2e VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
if(NOT EXISTS "${REPO_ROOT}/JUCE/CMakeLists.txt")
    message(FATAL_ERROR "JUCE submodule not found at ${REPO_ROOT}/JUCE. "
                        "Run: git submodule update --init --recursive")
endif()
if(WIN32)
    message(FATAL_ERROR "The sandbox e2e harness is POSIX-only (fd-passing host API).")
endif()
add_subdirectory("${REPO_ROOT}/JUCE" juce_build)

set(SANDBOX "${REPO_ROOT}/src/audio/Sandbox")
enable_testing()

# --- passthrough VST3 fixture (doubles its input) ---
juce_add_plugin(SlopPassThrough
    PRODUCT_NAME "SlopPassThrough"
    COMPANY_NAME "Slop"
    PLUGIN_MANUFACTURER_CODE Slop
    PLUGIN_CODE Sptp
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    VST3_CATEGORIES Fx)
target_sources(SlopPassThrough PRIVATE passthrough.cpp)
target_compile_definitions(SlopPassThrough PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(SlopPassThrough PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_basics
    juce::juce_audio_plugin_client)

# --- the real vst-host child (POSIX sources) ---
add_executable(slopsmith-vst-host
    "${REPO_ROOT}/src/vst-host/main.cpp"
    "${REPO_ROOT}/src/audio/VSTHost.cpp"
    "${SANDBOX}/Protocol.cpp"
    "${SANDBOX}/ControlChannel_shared.cpp" "${SANDBOX}/ControlChannel_posix.cpp"
    "${SANDBOX}/AudioChannel_shared.cpp"   "${SANDBOX}/AudioChannel_posix.cpp")
target_include_directories(slopsmith-vst-host PRIVATE "${REPO_ROOT}/src/audio")
target_link_libraries(slopsmith-vst-host PRIVATE
    juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats
    juce::juce_audio_processors juce::juce_core juce::juce_data_structures
    juce::juce_dsp juce::juce_events juce::juce_graphics juce::juce_gui_basics)
target_compile_definitions(slopsmith-vst-host PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_PLUGINHOST_LV2=0
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_STANDALONE_APPLICATION=1 JUCE_REPORT_APP_USAGE=0)
# main.cpp calls XInitThreads/XSetErrorHandler directly on Linux to install a
# non-fatal X error handler (JUCE only does this for standalone JUCEApplications,
# which this child is not). JUCE itself dlopen()s libX11, but our direct calls
# need it link-time. Mirrors src/vst-host/CMakeLists.txt.
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(slopsmith-vst-host PRIVATE ${X11_LIBRARIES})
    target_include_directories(slopsmith-vst-host PRIVATE ${X11_INCLUDE_DIR})
endif()

# --- host-side e2e driver ---
add_executable(sandbox_e2e_test
    e2e_test.cpp
    "${SANDBOX}/SandboxedProcessor.cpp"
    "${SANDBOX}/SandboxFactory_shared.cpp" "${SANDBOX}/SandboxFactory_posix.cpp"
    "${SANDBOX}/AudioChannel_shared.cpp"   "${SANDBOX}/AudioChannel_posix.cpp"
    "${SANDBOX}/ControlChannel_shared.cpp" "${SANDBOX}/ControlChannel_posix.cpp"
    "${SANDBOX}/SubprocessHandle_posix.cpp"
    "${SANDBOX}/Protocol.cpp")
target_include_directories(sandbox_e2e_test PRIVATE "${REPO_ROOT}/src/audio")
target_link_libraries(sandbox_e2e_test PRIVATE
    juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats
    juce::juce_audio_processors juce::juce_core juce::juce_dsp juce::juce_events)
target_compile_definitions(sandbox_e2e_test PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=0 JUCE_REPORT_APP_USAGE=0)
add_dependencies(sandbox_e2e_test slopsmith-vst-host SlopPassThrough_VST3)

# The VST3 bundle lands in <build>/SlopPassThrough_artefacts/<config>/VST3/.
add_test(NAME sandbox_e2e_test
    COMMAND sandbox_e2e_test
        "$<TARGET_FILE:slopsmith-vst-host>"
        "${CMAKE_BINARY_DIR}/SlopPassThrough_artefacts/$<CONFIG>/VST3/SlopPassThrough.vst3")

# --- in-process plugin fault-guard integration test ---
# Builds the REAL SignalChain against the full sandbox set and drives a
# deliberately-segfaulting in-process plugin through SignalChain::process();
# asserts invokePlugin()'s fault guard (POSIX siglongjmp / Windows SEH) keeps the
# host alive, releases the processor, and blocklists it. No subprocess / VST3
# fixture needed — the faulting plugin is an in-process AudioProcessor.
add_executable(signalchain_fault_test
    signalchain_fault_test.cpp
    "${REPO_ROOT}/src/audio/SignalChain.cpp"
    "${SANDBOX}/SandboxedProcessor.cpp"
    "${SANDBOX}/SandboxFactory_shared.cpp" "${SANDBOX}/SandboxFactory_posix.cpp"
    "${SANDBOX}/AudioChannel_shared.cpp"   "${SANDBOX}/AudioChannel_posix.cpp"
    "${SANDBOX}/ControlChannel_shared.cpp" "${SANDBOX}/ControlChannel_posix.cpp"
    "${SANDBOX}/SubprocessHandle_posix.cpp"
    "${SANDBOX}/Protocol.cpp")
target_include_directories(signalchain_fault_test PRIVATE "${REPO_ROOT}/src/audio")
target_link_libraries(signalchain_fault_test PRIVATE
    juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats
    juce::juce_audio_processors juce::juce_core juce::juce_data_structures
    juce::juce_dsp juce::juce_events)
target_compile_definitions(signalchain_fault_test PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=0 JUCE_REPORT_APP_USAGE=0
    # The guard INTENTIONALLY leaks the faulting processor (destructing it on a
    # corrupted heap is the very hazard it avoids), so JUCE's leak detector would
    # fire a Debug assertion at shutdown for a by-design leak. Disable it here.
    JUCE_CHECK_MEMORY_LEAKS=0)
add_test(NAME signalchain_fault_test COMMAND signalchain_fault_test)

# --- per-slot postGain unit test (PR #58) ---
# Drives a REAL SignalChain::process() with an identity in-process processor and
# asserts postGain scales output (all channels), rejects NaN, and is serialized
# by savePreset(). Same link set as signalchain_fault_test (SignalChain.cpp pulls
# in the sandbox blocklist symbols via invokePlugin's fault path).
add_executable(signalchain_postgain_test
    signalchain_postgain_test.cpp
    "${REPO_ROOT}/src/audio/SignalChain.cpp"
    "${SANDBOX}/SandboxedProcessor.cpp"
    "${SANDBOX}/SandboxFactory_shared.cpp" "${SANDBOX}/SandboxFactory_posix.cpp"
    "${SANDBOX}/AudioChannel_shared.cpp"   "${SANDBOX}/AudioChannel_posix.cpp"
    "${SANDBOX}/ControlChannel_shared.cpp" "${SANDBOX}/ControlChannel_posix.cpp"
    "${SANDBOX}/SubprocessHandle_posix.cpp"
    "${SANDBOX}/Protocol.cpp")
target_include_directories(signalchain_postgain_test PRIVATE "${REPO_ROOT}/src/audio")
target_link_libraries(signalchain_postgain_test PRIVATE
    juce::juce_audio_basics juce::juce_audio_devices juce::juce_audio_formats
    juce::juce_audio_processors juce::juce_core juce::juce_data_structures
    juce::juce_dsp juce::juce_events)
target_compile_definitions(signalchain_postgain_test PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=0 JUCE_REPORT_APP_USAGE=0)
add_test(NAME signalchain_postgain_test COMMAND signalchain_postgain_test)

# Orphan-cleanup regression (issue #265): host crash → child must not orphan.
# Linux-only — the driver's leak path + PR_SET_PDEATHSIG are JUCE_LINUX-gated;
# on macOS the env var is a no-op so this would assert clean-shutdown, not the
# crash path, which would be misleading. (UNIX AND NOT APPLE matches the X11
# linkage block above and excludes any other non-mac POSIX target.)
if(UNIX AND NOT APPLE)
    add_test(NAME sandbox_e2e_leak
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/leak_test.sh"
            "$<TARGET_FILE:sandbox_e2e_test>"
            "$<TARGET_FILE:slopsmith-vst-host>"
            "${CMAKE_BINARY_DIR}/SlopPassThrough_artefacts/$<CONFIG>/VST3/SlopPassThrough.vst3")
endif()
