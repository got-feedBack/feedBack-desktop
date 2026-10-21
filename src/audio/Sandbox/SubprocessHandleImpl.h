// SubprocessHandle::Impl — private OS process-handle wrapper, shared between
// SubprocessHandle_{win,posix}.cpp. Internal to those TUs; not part of the
// public header.

#pragma once

#include "SubprocessHandle.h"
#include "ThreadJoin.h"

#include <mutex>

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <sys/types.h>   // pid_t
#endif

namespace slopsmith::sandbox {

struct SubprocessHandle::Impl
{
    // Serialises the watcher join/detach in shutdown() across the (up to two)
    // concurrent callers — teardown() on the control I/O thread or watcher
    // thread, vs ~SubprocessHandle on the owner's thread. See ThreadJoin.h for
    // the double-join race this prevents.
    ThreadJoinState watcherJoin;
    // Serialises the handle-close tail of shutdown() between those callers.
    std::mutex closeMutex;

#if JUCE_WINDOWS
    PROCESS_INFORMATION pi{};
#else
    pid_t pid = -1;
#endif
};

} // namespace slopsmith::sandbox
