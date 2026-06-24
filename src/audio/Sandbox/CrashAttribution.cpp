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

bool endsWithVst3IgnoreCase(const wchar_t* p, size_t len) noexcept
{
    static const wchar_t ext[] = L".vst3";
    constexpr size_t el = 5; // wcslen(L".vst3")
    if (len < el) return false;
    const wchar_t* s = p + (len - el);
    for (size_t i = 0; i < el; ++i)
    {
        wchar_t a = s[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a + 32);
        if (a != ext[i]) return false;
    }
    return true;
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
        && info != nullptr && info->ExceptionRecord != nullptr
        && !g_writing.exchange(true, std::memory_order_acq_rel))
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
            // is a loaded .vst3, the fault is the plugin's — record it.
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
                if (len != 0 && len < cap && endsWithVst3IgnoreCase(pathW, len))
                    writeSentinel(pathW);
            }
        }
        // Leave g_writing set: a single attribution per process lifetime is all
        // we need, and not clearing it hardens against a fault inside the write.
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
    if (g_installed.exchange(false, std::memory_order_acq_rel))
        SetUnhandledExceptionFilter(g_prevFilter);
    g_sentinelPathW[0] = L'\0';
}

} // namespace slopsmith::sandbox

#else // ── non-Windows: no-op (POSIX SignalChain guard covers the armed path) ──

namespace slopsmith::sandbox {
void installVstCrashAttribution(const juce::String&) {}
void uninstallVstCrashAttribution() {}
} // namespace slopsmith::sandbox

#endif
