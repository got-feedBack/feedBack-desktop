// ControlChannel — Windows transport: a duplex named pipe in byte mode with
// overlapped I/O. The neutral request/reply/dispatch logic lives in
// ControlChannel_shared.cpp; this file implements createServerSide /
// connectClientSide / start / stop / waitForPeer / readFrame / writeFrame.
//
// Byte mode (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE) + the explicit length-prefix
// framing was chosen over PIPE_TYPE_MESSAGE because the sandbox's `ready`
// frame wasn't being delivered reliably in message mode — see commit 2cb9ae9.

#include "ControlChannelImpl.h"

#if ! JUCE_WINDOWS
 #error "ControlChannel_win.cpp is Windows-only; POSIX builds use ControlChannel_posix.cpp."
#endif

#include <thread>

#include "../VSTTrace.h"
#define CTL_TRACE(...) VST_TRACE("[ctrl] " __VA_ARGS__)

namespace slopsmith::sandbox {

bool ControlChannel::createServerSide(juce::String& pipeNameOut,
                                       juce::String& errorOut)
{
    juce::Uuid uuid;
    juce::String pipeName = "\\\\.\\pipe\\slopsmith-vst-" + uuid.toDashedString();

    // PIPE_REJECT_REMOTE_CLIENTS (Vista+) refuses connections from machines
    // other than the local one. The pipe name is random per-spawn, but
    // rejecting remote clients narrows the attack surface regardless.
    HANDLE h = CreateNamedPipeW(
        pipeName.toWideCharPointer(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
            | PIPE_REJECT_REMOTE_CLIENTS,
        /*maxInstances*/ 1,
        kControlPipeBufferBytes,
        kControlPipeBufferBytes,
        /*default timeout*/ 0,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        errorOut = "CreateNamedPipeW failed: " + juce::String((int)GetLastError());
        return false;
    }
    impl->pipe = h;
    impl->isServer = true;
    pipeNameOut = pipeName;
    return true;
}

bool ControlChannel::connectClientSide(const juce::String& pipeName,
                                        juce::String& errorOut)
{
    HANDLE h = CreateFileW(
        pipeName.toWideCharPointer(),
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);

    if (h == INVALID_HANDLE_VALUE)
    {
        errorOut = "CreateFileW (client) failed: " + juce::String((int)GetLastError());
        return false;
    }
    // Pipe was opened in BYTE mode (default) which matches our framing —
    // no SetNamedPipeHandleState call needed since CreateNamedPipeW on the
    // server side is now also PIPE_TYPE_BYTE | PIPE_READMODE_BYTE.
    impl->pipe = h;
    impl->isServer = false;
    return true;
}

bool ControlChannel::start(EventCallback evCb,
                            std::function<void(const juce::String&)> disconnectCb)
{
    lastStartError.clear();
    // Reassigning a joinable std::thread aborts via std::terminate, so refuse
    // a second start. Callers should stop() then re-create the channel.
    if (ioThread.joinable() || alive.load(std::memory_order_acquire))
    {
        lastStartError = "channel already started";
        return false;
    }
    if (!impl || impl->pipe == INVALID_HANDLE_VALUE)
    {
        lastStartError = "no pipe handle (createServerSide/connectClientSide not called or failed)";
        return false;
    }

    // Manual-reset so once stop() signals it, every subsequent wait inside
    // ioLoop returns immediately.
    impl->stopEvent = CreateEventW(nullptr, /*manualReset*/TRUE, FALSE, nullptr);
    if (impl->stopEvent == nullptr)
    {
        lastStartError = "CreateEventW(stopEvent) failed: GetLastError="
                       + juce::String((int)GetLastError());
        return false;
    }

    onEvent = std::move(evCb);
    onDisconnect = std::move(disconnectCb);
    alive.store(true, std::memory_order_release);
    // ConnectNamedPipe is performed inside the I/O thread (waitForPeer) so the
    // caller never blocks. If the sandbox subprocess dies before connecting,
    // the caller's watchdog can call stop(), which CancelIoEx's the pending
    // connect and unwinds cleanly.
    ioThread = std::thread([this] { ioLoop(); });
    return true;
}

void ControlChannel::stop()
{
    // Callback lifetime invariant: by the time stop() returns, both
    // `onEvent` and `onDisconnect` have been observed for the last time —
    // the I/O thread is either joined (non-self path) or has already
    // returned from its last dispatch (self-detach path; see below).
    // Owners therefore MUST call stop() before destroying any state
    // captured by-reference into onEvent/onDisconnect.
    alive.store(false, std::memory_order_release);

    // Signal stop BEFORE CancelIoEx. The race we're guarding against is
    // stop() running between ioThread spawn and the I/O thread issuing
    // ConnectNamedPipe — CancelIoEx would be a no-op there. With the
    // stop event signalled, the I/O thread's WaitForMultipleObjects exits
    // promptly regardless of whether the connect was ever started.
    if (impl && impl->stopEvent != nullptr)
        SetEvent(impl->stopEvent);

    // CancelIoEx unblocks the I/O thread's pending read so it can exit. The
    // handle must stay valid until the thread has returned — closing it
    // first is a TOCTOU on the in-flight read.
    if (impl && impl->pipe != INVALID_HANDLE_VALUE)
        CancelIoEx(impl->pipe, nullptr);

    // Serialised join/detach: stop() can run concurrently on the I/O thread
    // itself (failWith -> onDisconnect -> teardown -> stop) and on the owner's
    // thread (~ControlChannel — NOT covered by teardown's single-fire latch).
    // Two racing join() calls made the loser throw std::system_error (the
    // winner's join had closed the thread handle); the escaping exception
    // std::terminate'd the whole process. Self-stop is handled inside
    // joinThreadOnce as a detach. See ThreadJoin.h.
    joinThreadOnce(ioThread, impl->ioThreadJoin, "control-io");

    // The handle close is serialised too: two stop() callers can reach this
    // concurrently, and CloseHandle-ing the same handle twice risks closing a
    // recycled value.
    {
        std::lock_guard<std::mutex> lk(impl->closeMutex);
        if (impl->pipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(impl->pipe);
            impl->pipe = INVALID_HANDLE_VALUE;
        }
        if (impl->stopEvent != nullptr)
        {
            CloseHandle(impl->stopEvent);
            impl->stopEvent = nullptr;
        }
    }

    // Fail any in-flight requests so callers don't hang.
    std::lock_guard<std::mutex> lk(pendingMutex);
    for (auto& [id, p] : pending)
    {
        try { p->promise.set_value({}); }
        catch (const std::future_error&) {}
    }
    pending.clear();
}

// One-shot overlapped issue/wait/result, returning the actual bytes
// transferred (or 0 on failure with GetLastError set). Used by
// overlappedTransfer below in a loop, because byte-mode pipes can satisfy
// a single ReadFile/WriteFile with fewer bytes than requested.
static DWORD overlappedChunk(HANDLE pipe, bool isWrite, void* buf,
                             DWORD bytes, DWORD timeoutMs)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr)
        return 0;
    const BOOL started = isWrite
        ? WriteFile(pipe, buf, bytes, nullptr, &ov)
        : ReadFile (pipe, buf, bytes, nullptr, &ov);
    const DWORD startErr = started ? 0 : GetLastError();
    if (!started && startErr != ERROR_IO_PENDING)
    {
        CloseHandle(ov.hEvent);
        SetLastError(startErr);
        return 0;
    }
    if (timeoutMs != INFINITE)
    {
        if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0)
        {
            CancelIoEx(pipe, &ov);
            DWORD drained = 0;
            GetOverlappedResult(pipe, &ov, &drained, TRUE);
            CloseHandle(ov.hEvent);
            SetLastError(ERROR_TIMEOUT);
            return 0;
        }
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(pipe, &ov, &transferred, TRUE))
    {
        const DWORD err = GetLastError();
        CloseHandle(ov.hEvent);
        SetLastError(err);
        return 0;
    }
    CloseHandle(ov.hEvent);
    return transferred;
}

// Loop over overlappedChunk until `bytesPerOp` have been transferred. Needed
// because the pipe is in byte mode (PIPE_TYPE_BYTE), so a single ReadFile or
// WriteFile can return fewer bytes than requested even when the rest is
// still on the wire.
static bool overlappedTransfer(HANDLE pipe, bool isWrite, void* buf,
                               DWORD bytesPerOp, DWORD timeoutMs = INFINITE)
{
    auto* p = static_cast<char*>(buf);
    DWORD remaining = bytesPerOp;
    while (remaining > 0)
    {
        const DWORD got = overlappedChunk(pipe, isWrite, p, remaining,
                                          timeoutMs);
        if (got == 0)
            return false; // GetLastError() preserved from overlappedChunk
        p         += got;
        remaining -= got;
    }
    return true;
}

bool ControlChannel::writeFrame(const juce::MemoryBlock& body)
{
    std::lock_guard<std::mutex> lk(writeMutex);
    if (impl->pipe == INVALID_HANDLE_VALUE) return false;
    if (body.getSize() > kMaxControlMessageBytes) return false;

    constexpr DWORD kWriteTimeoutMs = 5000;
    uint32_t lenLE = (uint32_t)body.getSize();
    if (!overlappedTransfer(impl->pipe, true, &lenLE, sizeof(lenLE),
                            kWriteTimeoutMs))
        return false;
    if (body.getSize() > 0)
    {
        if (!overlappedTransfer(impl->pipe, true,
                                const_cast<void*>(body.getData()),
                                (DWORD)body.getSize(), kWriteTimeoutMs))
            return false;
    }
    return true;
}

// Classify a readFrame failure error code as a clean peer-side close vs a
// genuine I/O fault, so ControlChannel_shared.cpp's ioLoop stays free of Win32
// error codes.
static bool isPeerClosedError(unsigned long err)
{
    return err == ERROR_BROKEN_PIPE
        || err == ERROR_PIPE_NOT_CONNECTED
        || err == ERROR_NO_DATA;
}

bool ControlChannel::readFrame(juce::MemoryBlock& out)
{
    lastReadError = 0;
    lastReadPeerClosed = false;
    if (impl->pipe == INVALID_HANDLE_VALUE)
    {
        CTL_TRACE("readFrame: pipe is INVALID_HANDLE_VALUE");
        return false;
    }
    uint32_t lenLE = 0;
    if (!overlappedTransfer(impl->pipe, false, &lenLE, sizeof(lenLE)))
    {
        lastReadError = GetLastError();
        lastReadPeerClosed = isPeerClosedError(lastReadError);
        CTL_TRACE("readFrame: ReadFile(len) failed err=%lu", lastReadError);
        return false;
    }
    if (lenLE > kMaxControlMessageBytes)
    {
        lastReadError = ERROR_INVALID_DATA;
        CTL_TRACE("readFrame: oversized frame len=%lu", (unsigned long)lenLE);
        return false;
    }
    out.setSize(lenLE, false);
    if (lenLE == 0) return true;
    // Finite body timeout: once the 4-byte length prefix arrives the peer is
    // committed to sending lenLE more bytes; INFINITE here would wedge the I/O
    // thread if a buggy/malicious peer wrote the prefix and stalled. 30 s is
    // generous for the largest legitimate frame (state-restore can be a few
    // MB through a slow link) while bounding the DoS window.
    constexpr DWORD kBodyReadTimeoutMs = 30000;
    if (!overlappedTransfer(impl->pipe, false, out.getData(),
                            (DWORD)lenLE, kBodyReadTimeoutMs))
    {
        lastReadError = GetLastError();
        lastReadPeerClosed = isPeerClosedError(lastReadError);
        CTL_TRACE("readFrame: ReadFile(body len=%lu) failed err=%lu",
                  (unsigned long)lenLE, lastReadError);
        return false;
    }
    return true;
}

bool ControlChannel::waitForPeer(juce::String& failReason)
{
    // Server side: wait for the sandbox to connect. Overlapped because the
    // pipe was opened with FILE_FLAG_OVERLAPPED; synchronous wait via
    // GetOverlappedResult, racing the stop event.
    if (impl->pipe == INVALID_HANDLE_VALUE)
    {
        failReason = kReasonReadError + " (no pipe)";
        return false;
    }
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr)
    {
        CTL_TRACE("ConnectNamedPipe: CreateEventW failed err=%lu",
                  (unsigned long)GetLastError());
        failReason = kReasonReadError + " (event)";
        return false;
    }
    BOOL ok = ConnectNamedPipe(impl->pipe, &ov);
    DWORD err = ok ? 0 : GetLastError();
    if (!ok && err == ERROR_IO_PENDING)
    {
        // Wait on the connect event AND the stop event so a stop() during the
        // window between ioThread spawn and this call still unwinds (CancelIoEx
        // alone is a no-op against not-yet-issued I/O).
        HANDLE waits[2] = { ov.hEvent, impl->stopEvent };
        const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (which == WAIT_OBJECT_0 + 1)
        {
            CancelIoEx(impl->pipe, &ov);
            DWORD drained = 0;
            GetOverlappedResult(impl->pipe, &ov, &drained, TRUE);
            CloseHandle(ov.hEvent);
            CTL_TRACE("ConnectNamedPipe cancelled by stop()");
            failReason = kReasonReadError + " (stopped)";
            return false;
        }
        DWORD t = 0;
        ok = GetOverlappedResult(impl->pipe, &ov, &t, TRUE);
        if (!ok) err = GetLastError();
    }
    else if (!ok && err == ERROR_PIPE_CONNECTED)
    {
        ok = TRUE;
    }
    CloseHandle(ov.hEvent);
    if (!ok)
    {
        CTL_TRACE("ConnectNamedPipe failed err=%lu", (unsigned long)err);
        failReason = kReasonReadError + " (connect)";
        return false;
    }
    CTL_TRACE("ConnectNamedPipe returned (client connected)");
    return true;
}

} // namespace slopsmith::sandbox
