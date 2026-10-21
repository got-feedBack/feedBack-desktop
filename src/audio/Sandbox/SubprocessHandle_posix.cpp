// SubprocessHandle — POSIX backend (macOS + Linux).
//
// Spawn: posix_spawn, never a bare fork. The host process has already touched
// CoreAudio / the Obj-C runtime (it links them and runs the in-process VST
// path), and fork-without-immediate-exec is unsafe there — CoreFoundation
// aborts on many post-fork calls. posix_spawn does the fork+exec atomically.
//
// fd inheritance: posix_spawn_file_actions_adddup2 places each requested host
// fd onto a fixed child fd number (left non-close-on-exec by dup2). On macOS
// POSIX_SPAWN_CLOEXEC_DEFAULT forces every other fd close-on-exec, so ONLY the
// dup2()'d fds reach the child (the analog of Windows bInheritHandles=FALSE).
// Linux lacks that flag; the caller keeps its other fds CLOEXEC instead.
//
// Exit watcher: a blocking waitpid in a dedicated thread (mirrors the Windows
// WaitForSingleObject watcher). We deliberately do NOT install a global
// SIGCHLD handler — inside Electron that would fight libuv's own child reaping.
//
// Shutdown: SIGTERM, then SIGKILL after the timeout (the caller sends the
// `shutdown` control op first for a clean exit; this is the backstop).

#include "SubprocessHandleImpl.h"
#include "../VSTTrace.h"

#if JUCE_WINDOWS
 #error "SubprocessHandle_posix.cpp is POSIX-only; Windows builds use SubprocessHandle_win.cpp."
#endif

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
 #include <crt_externs.h>
 #define SLOPSMITH_ENVIRON (*_NSGetEnviron())
#else
 extern char** environ;
 #define SLOPSMITH_ENVIRON environ
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
    // The Windows start() inherits nothing and connects by name; on POSIX the
    // IPC objects are fd-passed, so callers must use startPosix with the fd
    // list. Forward to it with no inherited fds for API symmetry / a plain
    // "just run this exe" spawn.
    return startPosix(exePath, args, {}, std::move(onExit), errorOut);
}

bool SubprocessHandle::startPosix(const juce::String& exePath,
                                   const juce::StringArray& args,
                                   const std::vector<InheritedFd>& inherited,
                                   std::function<void(int)> onExit,
                                   juce::String& errorOut)
{
    // Refuse to re-spawn over a still-running process — reassigning a joinable
    // std::thread calls std::terminate, and we'd leak the prior pid.
    if (running.load(std::memory_order_acquire) || watcher.joinable())
    {
        errorOut = "subprocess already running — call shutdown() first";
        return false;
    }

    // Build argv: posix_spawn takes a plain char* const[] — no shell, no
    // quoting (the Windows CommandLineToArgvW quoting dance is gone). argv[0]
    // is the exe path by convention.
    std::vector<std::string> storage;
    storage.reserve((size_t)args.size() + 1);
    storage.push_back(exePath.toStdString());
    for (const auto& a : args)
        storage.push_back(a.toStdString());

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& s : storage)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    // Validate every file-action / attr setup call: a failure (bad fd,
    // resource pressure) must surface here, not as an opaque handshake timeout
    // after posix_spawn runs a child missing its dup2()'d fds.
    posix_spawn_file_actions_t actions;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0)
    {
        // init failed → `actions` is uninitialized; destroying it is undefined.
        errorOut = "posix_spawn_file_actions_init failed: "
                 + juce::String(strerror(rc));
        return false;
    }
    for (const auto& f : inherited)
        if (rc == 0)
            rc = posix_spawn_file_actions_adddup2(&actions, f.hostFd, f.childFd);
    if (rc != 0)
    {
        errorOut = "posix_spawn_file_actions setup failed: "
                 + juce::String(strerror(rc));
        posix_spawn_file_actions_destroy(&actions);   // init succeeded → safe
        return false;
    }

    posix_spawnattr_t attr;
    int arc = posix_spawnattr_init(&attr);
    if (arc != 0)
    {
        // init failed → don't destroy `attr`; `actions` was inited, so free it.
        errorOut = "posix_spawnattr_init failed: " + juce::String(strerror(arc));
        posix_spawn_file_actions_destroy(&actions);
        return false;
    }
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    // macOS: only the dup2()'d fds above survive into the child. On Linux this
    // flag doesn't exist; CLOEXEC hygiene on the host fds covers it instead.
    arc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    if (arc != 0)
    {
        errorOut = "posix_spawnattr setup failed: " + juce::String(strerror(arc));
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attr);               // init succeeded → safe
        return false;
    }

    pid_t pid = -1;
    VST_TRACE("SubprocessHandle.startPosix: posix_spawn '%s' (%d inherited fds)",
              exePath.toRawUTF8(), (int)inherited.size());
    rc = posix_spawn(&pid, exePath.toRawUTF8(), &actions, &attr,
                     argv.data(), SLOPSMITH_ENVIRON);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);

    if (rc != 0)
    {
        errorOut = "posix_spawn failed: " + juce::String(strerror(rc));
        VST_TRACE("SubprocessHandle.startPosix: posix_spawn FAILED rc=%d (%s)",
                  rc, strerror(rc));
        return false;
    }
    VST_TRACE("SubprocessHandle.startPosix: spawned pid=%d", (int)pid);
    impl->pid = pid;
    cachedPid = (uint32_t)pid;
    running.store(true, std::memory_order_release);
    // onExitCb is single-writer (this path) and read only by the watcher
    // thread spawned just below — same invariant the Windows backend relies on.
    onExitCb = std::move(onExit);

    watcher = std::thread([this, pid]
    {
        // Thread entry point: never let an exception escape — an uncaught
        // C++ exception calls std::terminate and kills the whole host process.
        try
        {
            int status = 0;
            int code = -1;
            for (;;)
            {
                const pid_t w = ::waitpid(pid, &status, 0);
                if (w == pid)
                {
                    if (WIFEXITED(status))        code = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
                    break;
                }
                if (w < 0)
                {
                    if (errno == EINTR) continue;
                    // ECHILD: someone else reaped it (e.g. libuv's SIGCHLD handler
                    // inside Electron). Treat as exited with unknown status rather
                    // than spinning — see the kqueue/EVFILT_PROC fallback note in
                    // the design plan if this proves common.
                    break;
                }
            }
            running.store(false, std::memory_order_release);
            if (onExitCb) onExitCb(code);
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
    if (running.load(std::memory_order_acquire) && impl->pid > 0)
    {
        // Escalating close: SIGTERM, wait for the watcher to observe the exit
        // (it owns waitpid — we must NOT waitpid here too or we race the reap),
        // then SIGKILL. Poll `running` rather than waitpid.
        ::kill(impl->pid, SIGTERM);

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(timeoutMs);
        while (running.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));

        if (running.load(std::memory_order_acquire))
            ::kill(impl->pid, SIGKILL);
    }

    // Serialised join/detach — same double-join race as the Windows backend;
    // see ThreadJoin.h and SubprocessHandle_win.cpp::shutdown.
    joinThreadOnce(watcher, impl->watcherJoin, "subprocess-watcher");

    std::lock_guard<std::mutex> lk(impl->closeMutex);
    impl->pid = -1;
}

} // namespace slopsmith::sandbox
