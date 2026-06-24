#include "CrashAttribution.h"
#include "../VSTTrace.h"

#if JUCE_WINDOWS

#include <windows.h>
#include <atomic>

namespace slopsmith::sandbox {
namespace {

// STATUS_STACK_BUFFER_OVERRUN (__fastfail / __security_check_cookie) isn't named
// in <windows.h>; it's the code raised by /GS and many fatal aborts.
constexpr DWORD kStatusStackBufferOverrun = 0xC0000409u;

// Sentinel path captured at install, as a fixed UTF-16 buffer so the filter
// touches no heap while the process is faulting.
wchar_t g_sentinelPathW[1024] = { 0 };

LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
std::atomic<bool> g_installed{ false };
// Re-entrancy guard: if the sentinel write itself faulted we must not recurse.
std::atomic<bool> g_writing{ false };

// Resolve a loaded-module path to its enclosing `.vst3` BUNDLE path, IN PLACE,
// so it matches the blocklist key (shouldSandbox/setCrashedPlugins key on
// desc.fileOrIdentifier = the bundle directory). A Windows VST3 bundle is loaded
// via its inner DLL (`Foo.vst3\Contents\x86_64-win\Foo.vst3`), so
// GetModuleFileNameW returns that inner path; truncate at the first `.vst3`
// path-component boundary to recover `…\Foo.vst3`. A single-file `.vst3` already
// ends there and is left unchanged. Returns false (→ not a VST3 → skip) when no
// `.vst3` component is present. Case-insensitive (ASCII), allocation-free.
bool truncateToVst3Bundle(wchar_t* p) noexcept
{
    static const wchar_t ext[] = L".vst3";
    for (size_t i = 0; p[i] != L'\0'; ++i)
    {
        size_t k = 0;
        for (; k < 5; ++k)
        {
            wchar_t a = p[i + k];
            if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a + 32);
            if (a != ext[k]) break;
        }
        if (k == 5)
        {
            const wchar_t after = p[i + 5];
            if (after == L'\0' || after == L'\\' || after == L'/')
            {
                p[i + 5] = L'\0'; // keep "…\Foo.vst3", drop any \Contents\… tail
                return true;
            }
        }
    }
    return false;
}

// Allocation-free write of {"plugin":"<json-escaped>","op":"native-crash"} —
// the exact shape src/main/vst-crash-guard.ts reads from the sentinel. Called
// from the unhandled-exception filter on the faulting thread, so it uses only
// stack buffers + raw Win32 file I/O.
void writeSentinel(const wchar_t* modulePathW) noexcept
{
    char utf8[1024];
    const int n = WideCharToMultiByte(CP_UTF8, 0, modulePathW, -1,
                                      utf8, static_cast<int>(sizeof(utf8)) - 1,
                                      nullptr, nullptr);
    if (n <= 0) return;

    char json[1400];
    size_t j = 0;
    const auto put = [&](const char* s) {
        while (*s && j < sizeof(json) - 1) json[j++] = *s++;
    };
    put("{\"plugin\":\"");
    for (int i = 0; utf8[i] != '\0' && j < sizeof(json) - 24; ++i)
    {
        const char c = utf8[i];
        if (c == '\\' || c == '"') { json[j++] = '\\'; json[j++] = c; }
        else if (c == '\r' || c == '\n' || c == '\t') { /* drop control chars */ }
        else json[j++] = c;
    }
    put("\",\"op\":\"native-crash\"}");

    const HANDLE h = CreateFileW(g_sentinelPathW, GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                 nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, json, static_cast<DWORD>(j), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS* info) noexcept
{
    // SetUnhandledExceptionFilter only fires for genuinely UNHANDLED exceptions
    // (last chance), so a plugin that first-chance-faults-and-handles never
    // reaches here — no false attribution and no per-exception I/O.
    if (g_installed.load(std::memory_order_acquire)
        && g_sentinelPathW[0] != L'\0'
        && info != nullptr && info->ExceptionRecord != nullptr)
    {
        const DWORD code = info->ExceptionRecord->ExceptionCode;
        const bool fatalFault =
               code == EXCEPTION_ACCESS_VIOLATION
            || code == EXCEPTION_ILLEGAL_INSTRUCTION
            || code == EXCEPTION_PRIV_INSTRUCTION
            || code == EXCEPTION_IN_PAGE_ERROR
            || code == kStatusStackBufferOverrun;
        if (fatalFault)
        {
            // Map the faulting instruction to its owning module. If that module
            // is a loaded .vst3, the fault is (heuristically — by faulting
            // address, with no host-frame corroboration) the plugin's, so
            // record it. A corrupted control transfer INTO plugin code can
            // mis-attribute; the cost is a good plugin forced to the sandbox,
            // never a crash, so the heuristic is acceptable here.
            HMODULE mod = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(
                                       info->ExceptionRecord->ExceptionAddress),
                                   &mod)
                && mod != nullptr)
            {
                wchar_t pathW[1024];
                const DWORD cap = static_cast<DWORD>(sizeof(pathW) / sizeof(pathW[0]));
                const DWORD len = GetModuleFileNameW(mod, pathW, cap);
                // Gate the one-shot on a CONFIRMED VST3 fault, not on merely
                // reaching the filter: a non-VST3 unhandled exception (or a
                // concurrent benign one) must not burn the latch and disable
                // attribution for the real plugin fault. The exchange also
                // serialises two threads faulting in plugins at once + guards
                // against a fault inside writeSentinel re-entering.
                if (len != 0 && len < cap && truncateToVst3Bundle(pathW)
                    && !g_writing.exchange(true, std::memory_order_acq_rel))
                    writeSentinel(pathW);
            }
        }
    }

    // Never handle — defer to the previously installed top-level filter
    // (Crashpad) so the dump is still produced and the process terminates as it
    // otherwise would.
    return g_prevFilter != nullptr ? g_prevFilter(info)
                                   : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void installVstCrashAttribution(const juce::String& sentinelPath)
{
    if (sentinelPath.isEmpty()) return;

    // Copy the path into the fixed buffer (manual, to avoid CRT-secure deps).
    const wchar_t* wide = sentinelPath.toWideCharPointer();
    const size_t cap = sizeof(g_sentinelPathW) / sizeof(g_sentinelPathW[0]) - 1;
    size_t i = 0;
    for (; wide[i] != L'\0' && i < cap; ++i) g_sentinelPathW[i] = wide[i];
    g_sentinelPathW[i] = L'\0';

    // Install once, chaining to whatever filter is already in place (Crashpad).
    if (!g_installed.exchange(true, std::memory_order_acq_rel))
        g_prevFilter = SetUnhandledExceptionFilter(unhandledFilter);

    VST_TRACE("installVstCrashAttribution: armed");
}

void uninstallVstCrashAttribution()
{
    // Clearing g_installed (acquire-load gated in the filter) is what disarms
    // the write path; we deliberately do NOT mutate g_sentinelPathW here so a
    // fault racing this teardown can't read a half-zeroed path.
    if (g_installed.exchange(false, std::memory_order_acq_rel))
        SetUnhandledExceptionFilter(g_prevFilter);
}

} // namespace slopsmith::sandbox

#else // ── non-Windows: no-op (POSIX SignalChain guard covers the armed path) ──

namespace slopsmith::sandbox {
void installVstCrashAttribution(const juce::String&) {}
void uninstallVstCrashAttribution() {}
} // namespace slopsmith::sandbox

#endif
