// SubprocessHandle — Windows backend (CreateProcessW + WM_QUIT/TerminateProcess
// shutdown + WaitForSingleObject exit watcher). POSIX lives in
// SubprocessHandle_posix.cpp.

#include "SubprocessHandleImpl.h"
#include "../VSTTrace.h"

#if ! JUCE_WINDOWS
 #error "SubprocessHandle_win.cpp is Windows-only; POSIX builds use SubprocessHandle_posix.cpp."
#endif

namespace slopsmith::sandbox {

SubprocessHandle::SubprocessHandle() : impl(std::make_unique<Impl>()) {}

SubprocessHandle::~SubprocessHandle()
{
    shutdown(2000);
}

bool SubprocessHandle::start(const juce::String& exePath,
                              const juce::StringArray& args,
                              std::function<void(int)> onExit,
                              juce::String& errorOut)
{
    // Refuse to re-spawn over a still-running process — overwriting impl->pi
    // would leak the existing process/thread handles, and reassigning a
    // joinable std::thread calls std::terminate.
    if (running.load(std::memory_order_acquire) || watcher.joinable())
    {
        errorOut = "subprocess already running — call shutdown() first";
        return false;
    }

    // Win32 CommandLineToArgvW quoting rules per Microsoft docs:
    //   - 2N backslashes followed by `"` → N backslashes + end of quoted region
    //   - 2N+1 backslashes followed by `"` → N backslashes + literal `"`
    //   - backslashes NOT followed by `"` are kept literal
    // So embedded `"` needs all preceding backslashes doubled AND the `"`
    // backslash-escaped, and a trailing backslash inside a quoted arg also
    // needs to be doubled (otherwise it escapes the closing quote).
    auto quoteWin32 = [](const juce::String& in) -> juce::String
    {
        juce::String out;
        out << '"';
        int backslashes = 0;
        for (juce::juce_wchar c : in)
        {
            if (c == '\\')
            {
                ++backslashes;
            }
            else if (c == '"')
            {
                // Double all the pending backslashes, then escape the quote.
                out += juce::String::repeatedString("\\\\", backslashes);
                out += "\\\"";
                backslashes = 0;
            }
            else
            {
                // Pending backslashes are literal (not followed by a quote).
                out += juce::String::repeatedString("\\", backslashes);
                backslashes = 0;
                out += juce::String::charToString(c);
            }
        }
        // Trailing backslashes inside a quoted arg get doubled, otherwise
        // the closing quote turns into an escaped literal `"`.
        out += juce::String::repeatedString("\\\\", backslashes);
        out << '"';
        return out;
    };

    juce::String cmd;
    cmd << quoteWin32(exePath);
    for (auto& a : args)
        cmd << ' ' << quoteWin32(a);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // detach console; the sandbox is GUI-only

    std::wstring wcmd = cmd.toWideCharPointer();
    VST_TRACE("SubprocessHandle.start: CreateProcessW cmd='%s'", cmd.toRawUTF8());
    if (!CreateProcessW(
            nullptr, wcmd.data(),
            nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            nullptr, nullptr,
            &si, &impl->pi))
    {
        DWORD err = GetLastError();
        errorOut = "CreateProcessW failed: " + juce::String((int)err);
        VST_TRACE("SubprocessHandle.start: CreateProcessW FAILED err=%lu", (unsigned long)err);
        return false;
    }
    VST_TRACE("SubprocessHandle.start: spawned pid=%lu",
              (unsigned long)impl->pi.dwProcessId);
    running.store(true, std::memory_order_release);
    cachedPid = (uint32_t)impl->pi.dwProcessId;
    // `onExitCb` is single-writer: only this start() path assigns to it,
    // and the watcher thread (the only reader) is spawned a few lines
    // below, after the assignment. The early-return guard at the top of
    // this function rejects re-starts while a previous run is still
    // alive, which keeps that invariant intact. If a future refactor
    // ever permits an in-flight re-start, this assignment vs. the
    // watcher's read becomes a data race — make onExitCb atomic or
    // serialise via a mutex at that point.
    onExitCb = std::move(onExit);

    HANDLE procHandle = impl->pi.hProcess;
    watcher = std::thread([this, procHandle]
    {
        // Thread entry point: never let an exception escape — an uncaught
        // C++ exception calls std::terminate and kills the whole host process.
        try
        {
            WaitForSingleObject(procHandle, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(procHandle, &code);
            running.store(false, std::memory_order_release);
            if (onExitCb) onExitCb((int)code);
        }
        catch (...)
        {
            VST_TRACE("SubprocessHandle watcher: swallowed exception");
            running.store(false, std::memory_order_release);
        }
    });
    return true;
}

void SubprocessHandle::shutdown(int timeoutMs)
{
    if (running.load(std::memory_order_acquire))
    {
        // Try a clean shutdown: post WM_QUIT to the subprocess's initial
        // thread (`pi.dwThreadId` from PROCESS_INFORMATION). PostThreadMessageW
        // is per-TID, not per-process — this works because vst-host's WinMain
        // runs the JUCE message loop on the initial thread (the audio worker
        // is a child thread that doesn't pump messages), so WM_QUIT lands on
        // the right pump. If a future refactor moves the message loop off the
        // initial thread, this needs the new TID or to switch to a
        // process-wide signalling mechanism (named event, etc.). If
        // dwThreadId is zero (start() failed mid-way) we skip and let the
        // wait+TerminateProcess below clean up.
        if (impl->pi.dwThreadId != 0)
            PostThreadMessageW(impl->pi.dwThreadId, WM_QUIT, 0, 0);

        DWORD wait = WaitForSingleObject(impl->pi.hProcess, (DWORD)timeoutMs);
        if (wait != WAIT_OBJECT_0)
            TerminateProcess(impl->pi.hProcess, 1);
    }

    // Serialised join/detach: shutdown() can run concurrently on the control
    // I/O thread (teardown via failWith -> onDisconnect) and on the owner's
    // thread (~SubprocessHandle — NOT covered by teardown's single-fire latch).
    // Two racing join() calls made the loser throw std::system_error(ESRCH)
    // (the winner's join had closed the thread handle); the escaping exception
    // std::terminate'd the whole process. Self-shutdown (watcher thread) is
    // handled inside joinThreadOnce as a detach — safe because the watcher
    // touches no member state after onExitCb returns. See ThreadJoin.h.
    joinThreadOnce(watcher, impl->watcherJoin, "subprocess-watcher");

    // Always close handles — when the watcher detected a crash, `running` is
    // already false here, but the kernel handles are still ours to release.
    // Serialised: two shutdown() callers can reach this concurrently, and
    // CloseHandle-ing the same handle twice risks closing a recycled value.
    {
        std::lock_guard<std::mutex> lk(impl->closeMutex);
        if (impl->pi.hThread)  { CloseHandle(impl->pi.hThread);  impl->pi.hThread = nullptr; }
        if (impl->pi.hProcess) { CloseHandle(impl->pi.hProcess); impl->pi.hProcess = nullptr; }
    }
}

} // namespace slopsmith::sandbox
