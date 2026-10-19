// ControlChannel::Impl — private OS-handle wrapper shared between the
// platform-neutral request/reply/dispatch logic (ControlChannel_shared.cpp)
// and the per-platform transport (ControlChannel_{win,posix}.cpp).
//
// Internal to the ControlChannel translation units; NOT part of the public
// ControlChannel.h surface (keeps <windows.h> / POSIX fd semantics out of
// every includer).

#pragma once

#include "ControlChannel.h"
#include "ThreadJoin.h"

#include <mutex>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace slopsmith::sandbox {

struct ControlChannel::Impl
{
    bool isServer = false;

    // Serialises the ioThread join/detach in stop() across concurrent callers
    // (teardown on the I/O thread itself via failWith -> onDisconnect, vs
    // ~ControlChannel on the owner's thread) — see ThreadJoin.h.
    ThreadJoinState ioThreadJoin;
    // Serialises the handle/fd close tail of stop() between those callers.
    std::mutex closeMutex;

#if JUCE_WINDOWS
    HANDLE pipe = INVALID_HANDLE_VALUE;
    // Set by stop() before the join. The I/O thread's ConnectNamedPipe wait
    // observes it via WaitForMultipleObjects so a stop() that races the start
    // of ioLoop still tears down promptly — CancelIoEx alone is a no-op
    // against I/O that hasn't been issued yet.
    HANDLE stopEvent = nullptr;
#else
    // Connected stream socket (one end of a socketpair). Bidirectional, like
    // the Windows duplex named pipe; the [u32 length-LE][body] framing layer
    // in ControlChannel_shared.cpp sits on top unchanged.
    int fd = -1;

    // Self-pipe used to break the I/O thread out of poll() on stop(). stop()
    // writes one byte and never drains it (manual-reset semantics: every
    // subsequent poll sees POLLIN and returns immediately), mirroring the
    // Windows manual-reset stopEvent.
    int stopPipe[2] = { -1, -1 };

    // Server side only: the sandbox's end of the socketpair, produced by
    // createServerSide and consumed (owned) by whoever connects — an
    // in-process peer via connectClientSideFd, or the spawner which dup2()s
    // it into the child. Exposed via ControlChannel::sandboxFd(). NOT closed
    // by stop(): ownership has transferred to the consumer by then.
    int handoffFd = -1;
#endif
};

} // namespace slopsmith::sandbox
