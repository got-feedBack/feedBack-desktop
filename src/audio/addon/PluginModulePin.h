#pragma once

// Plugin-module pinning — P0 item 4 of the audio architecture guide (§12).
//
// JUCE refcounts VST3 modules and unloads them when the last instance dies;
// a window/timer message already queued to that module's code then fires
// into unmapped or reused pages — the CFG fail-fast
// (0xc0000409 / FAST_FAIL_GUARD_ICALL_CHECK_FAILURE) in the 2026-07-17
// field dumps. Pinning tells the loader to never unload the module for the
// life of the process.
//
// Header-only ON PURPOSE: VSTHost.cpp is compiled into four different
// targets across three CMake projects (the addon, slopsmith-vst-host in
// two projects, slopsmith-vst-scan on macOS); an out-of-line definition
// broke every link that didn't add the extra .cpp (PR #120 CI).
//
// The pin matches loaded modules by PATH, not base name: the identifier we
// get is usually the bundle directory (…\Foo.vst3\), while the loader knows
// the inner …\Contents\x86_64-win\Foo.vst3 file — and two plugins may share
// a base name. Enumerate loaded modules and pin every one whose full path
// starts with the normalized identifier (K32* exports so no psapi.lib).
// No-op off Windows and for modules that are not currently loaded (e.g.
// sandboxed plugins — the child process owns those).

#include <cstdio>

#if defined(_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 // PSAPI_VERSION 2 maps EnumProcessModules/GetModuleFileNameExW onto the
 // kernel32-exported K32* variants — no psapi.lib link needed (matters for
 // the extra targets this header serves; see the header-only note above).
 #ifndef PSAPI_VERSION
  #define PSAPI_VERSION 2
 #endif
 #include <psapi.h>
 #include <vector>
 #include <cwctype>
#endif

#include <juce_core/juce_core.h>

namespace slopsmith::addon {

inline void pinPluginModuleForever(const char* fileOrIdentifierUtf8)
{
#if defined(_WIN32)
    const juce::String wanted =
        juce::String::fromUTF8(fileOrIdentifierUtf8).replaceCharacter('/', '\\');
    if (wanted.isEmpty())
        return;

    auto startsWithIgnoreCase = [](const wchar_t* full, const wchar_t* prefix) {
        while (*prefix != 0)
        {
            if (*full == 0
                || std::towlower(static_cast<wint_t>(*full))
                       != std::towlower(static_cast<wint_t>(*prefix)))
                return false;
            ++full;
            ++prefix;
        }
        return true;
    };

    std::vector<HMODULE> modules(1024);
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules.data(),
                               static_cast<DWORD>(modules.size() * sizeof(HMODULE)),
                               &needed))
        return;
    modules.resize(needed / sizeof(HMODULE));

    for (HMODULE mod : modules)
    {
        wchar_t modPath[MAX_PATH * 2] = {};
        if (GetModuleFileNameExW(GetCurrentProcess(), mod, modPath,
                                    static_cast<DWORD>(sizeof(modPath) / sizeof(modPath[0]))) == 0)
            continue;
        if (!startsWithIgnoreCase(modPath, wanted.toWideCharPointer()))
            continue;

        HMODULE pinned = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, modPath, &pinned))
            fprintf(stderr, "[lifecycle] pinned plugin module '%s' for process "
                            "lifetime\n",
                    juce::String(modPath).toRawUTF8());
    }
#else
    (void) fileOrIdentifierUtf8;
#endif
}

} // namespace slopsmith::addon
