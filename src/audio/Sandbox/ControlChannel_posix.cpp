// ControlChannel — POSIX transport (macOS + Linux).
//
// Transport is a connected AF_UNIX SOCK_STREAM socket (one end of a
// socketpair). It is bidirectional like the Windows duplex named pipe, has the
// same partial-read/partial-write stream semantics (so the [u32 length-LE]
// [body] framing in ControlChannel_shared.cpp carries over unchanged), and is
// passed to the sandbox by fd inheritance rather than re-opened by name —
// dodging the macOS sun_path length limit and leaving no socket file to leak.
//
// The I/O thread multiplexes the socket and a self-pipe with poll(): stop()
// writes one (never-drained) byte to the self-pipe, which is the POSIX analog
// of the Windows manual-reset stopEvent + CancelIoEx — every subsequent poll
// returns immediately so the thread unwinds promptly.
//
// SIGPIPE: writing to a socket whose peer has closed would raise SIGPIPE and
// kill the process by default (Windows just returns an error). Every send()
// uses MSG_NOSIGNAL where available and the fd carries SO_NOSIGPIPE on macOS,
// so a dead peer surfaces as EPIPE — never a signal.

#include "ControlChannelImpl.h"

#if JUCE_WINDOWS
 #error "ControlChannel_posix.cpp is POSIX-only; Windows builds use ControlChannel_win.cpp."
#endif

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../VSTTrace.h"
#define CTL_TRACE(...) VST_TRACE("[ctrl] " __VA_ARGS__)

namespace slopsmith::sandbox {

namespace {

void setNonBlocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void setCloExec(int fd)
{
    const int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void setNoSigPipe([[maybe_unused]] int fd)
{
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

ssize_t sendNoSignal(int fd, const void* buf, size_t n)
{
#ifdef MSG_NOSIGNAL
    return ::send(fd, buf, n, MSG_NOSIGNAL);
#else
    return ::send(fd, buf, n, 0); // SO_NOSIGPIPE set on the fd instead
#endif
}

} // namespace

bool ControlChannel::createServerSide(juce::String& pipeNameOut,
                                       juce::String& errorOut)
{
    int sp[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0)
    {
        errorOut = "socketpair failed: " + juce::String(strerror(errno));
        return false;
    }
    impl->fd = sp[0];          // host end (stays here)
    impl->handoffFd = sp[1];   // sandbox end (owned by whoever connects)
    impl->isServer = true;
    setNonBlocking(impl->fd);
    setCloExec(impl->fd);
    setNoSigPipe(impl->fd);
    setCloExec(impl->handoffFd);
    setNoSigPipe(impl->handoffFd);
    pipeNameOut.clear();       // no named object on POSIX
    return true;
}

int ControlChannel::sandboxFd() const noexcept
{
    return impl ? impl->handoffFd : -1;
}

void ControlChannel::closeSandboxFd() noexcept
{
    if (impl && impl->handoffFd >= 0)
    {
        ::close(impl->handoffFd);
        impl->handoffFd = -1;
    }
}

bool ControlChannel::connectClientSide(const juce::String& /*pipeName*/,
                                        juce::String& errorOut)
{
    // POSIX has no named control object — the sandbox adopts the inherited fd
    // via connectClientSideFd instead.
    errorOut = "connectClientSide(name) is Windows-only; use connectClientSideFd on POSIX";
    return false;
}

bool ControlChannel::connectClientSideFd(int fd, juce::String& errorOut)
{
    if (fd < 0)
    {
        errorOut = "connectClientSideFd: invalid fd";
        return false;
    }
    impl->fd = fd;             // take ownership
    impl->isServer = false;
    setNonBlocking(impl->fd);
    setCloExec(impl->fd);
    setNoSigPipe(impl->fd);
    return true;
}

bool ControlChannel::start(EventCallback evCb,
                            std::function<void(const juce::String&)> disconnectCb)
{
    lastStartError.clear();
    if (ioThread.joinable() || alive.load(std::memory_order_acquire))
    {
        lastStartError = "channel already started";
        return false;
    }
    if (!impl || impl->fd < 0)
    {
        lastStartError = "no socket (createServerSide/connectClientSideFd not called or failed)";
        return false;
    }

    // Self-pipe used as the stop signal. Read end stays readable forever once
    // stop() writes a byte (never drained) — manual-reset semantics matching
    // the Windows stopEvent.
    if (::pipe(impl->stopPipe) != 0)
    {
        lastStartError = "pipe(stopPipe) failed: " + juce::String(strerror(errno));
        return false;
    }
    setNonBlocking(impl->stopPipe[0]);
    setCloExec(impl->stopPipe[0]);
    setCloExec(impl->stopPipe[1]);

    onEvent = std::move(evCb);
    onDisconnect = std::move(disconnectCb);
    alive.store(true, std::memory_order_release);
    ioThread = std::thread([this] { ioLoop(); });
    return true;
}

void ControlChannel::stop()
{
    // See the Windows backend for the full callback-lifetime invariant: by the
    // time stop() returns the I/O thread has been joined (or detached on the
    // self-stop path) and onDisconnect has fired for the last time.
    alive.store(false, std::memory_order_release);

    // Signal stop FIRST (self-pipe byte, never drained), then shutdown() the
    // socket so a blocked recv()/poll() on it also wakes. Ordering mirrors the
    // Windows SetEvent-before-CancelIoEx: the self-pipe covers the window
    // before the I/O thread has even reached its first poll().
    if (impl && impl->stopPipe[1] >= 0)
    {
        const unsigned char b = 1;
        ssize_t r = ::write(impl->stopPipe[1], &b, 1);
        (void)r; // best-effort wake; a full self-pipe already means "stopping"
    }
    if (impl && impl->fd >= 0)
        ::shutdown(impl->fd, SHUT_RDWR);

    // Serialised join/detach — same double-join race as the Windows backend;
    // see ThreadJoin.h and ControlChannel_win.cpp::stop.
    joinThreadOnce(ioThread, impl->ioThreadJoin, "control-io");

    {
        std::lock_guard<std::mutex> lk(impl->closeMutex);
        if (impl->fd >= 0)          { ::close(impl->fd);          impl->fd = -1; }
        if (impl->stopPipe[0] >= 0) { ::close(impl->stopPipe[0]); impl->stopPipe[0] = -1; }
        if (impl->stopPipe[1] >= 0) { ::close(impl->stopPipe[1]); impl->stopPipe[1] = -1; }
        // handoffFd ownership transferred to the consumer at connect/spawn
        // time; do NOT close it here.
    }

    std::lock_guard<std::mutex> lk(pendingMutex);
    for (auto& [id, p] : pending)
    {
        try { p->promise.set_value({}); }
        catch (const std::future_error&) {}
    }
    pending.clear();
}

namespace {

// Transfer exactly `n` bytes to/from the socket, blocking via poll() up to
// `timeoutMs` (or indefinitely if INFINITE-style negative), while also waking
// on the stop self-pipe. Returns 0 on success, or a negative code:
//   -1 timeout, -2 stop requested, -3 peer closed (EOF/EPIPE/ECONNRESET),
//   -4 other I/O error.
constexpr int kXferOk        =  0;
constexpr int kXferTimeout   = -1;
constexpr int kXferStopped   = -2;
constexpr int kXferPeerClose = -3;
constexpr int kXferError     = -4;

int transferN(int fd, int stopFd, bool isWrite, void* buf, size_t n,
              int timeoutMs)
{
    auto* p = static_cast<char*>(buf);
    size_t remaining = n;
    const bool bounded = (timeoutMs >= 0);
    // Track elapsed (now - start) rather than an absolute now+timeout deadline:
    // juce::Time::getMillisecondCounter() is a 32-bit counter that wraps every
    // ~49.7 days, and an absolute deadline that straddles the wrap reads as
    // already-past (spurious immediate timeout). Unsigned (now - start) stays
    // correct across a single wrap for any timeout shorter than the wrap period.
    const uint32_t startMs = juce::Time::getMillisecondCounter();

    while (remaining > 0)
    {
        int waitMs = -1;
        if (bounded)
        {
            const uint32_t elapsed = juce::Time::getMillisecondCounter() - startMs;
            if (elapsed >= (uint32_t)timeoutMs) { errno = ETIMEDOUT; return kXferTimeout; }
            waitMs = (int)((uint32_t)timeoutMs - elapsed);
        }

        struct pollfd pfds[2]{};
        pfds[0].fd = fd;
        pfds[0].events = isWrite ? POLLOUT : POLLIN;
        pfds[1].fd = stopFd;
        pfds[1].events = POLLIN;
        const int nfds = (stopFd >= 0) ? 2 : 1;

        const int rc = ::poll(pfds, (nfds_t)nfds, waitMs);
        if (rc < 0)
        {
            if (errno == EINTR) continue;
            return kXferError;
        }
        // Timeout carries no syscall errno; set ETIMEDOUT so readFrame records a
        // deterministic lastReadError rather than a stale unrelated value.
        if (rc == 0) { errno = ETIMEDOUT; return kXferTimeout; }
        if (nfds == 2 && (pfds[1].revents & POLLIN)) return kXferStopped;

        // Attempt the transfer even on POLLHUP/POLLERR: a reader should still
        // drain buffered bytes before observing EOF.
        const ssize_t got = isWrite ? sendNoSignal(fd, p, remaining)
                                    : ::recv(fd, p, remaining, 0);
        if (got > 0)
        {
            p += got;
            remaining -= (size_t)got;
            continue;
        }
        if (got == 0)
        {
            // Clean EOF carries no errno; clear it so readFrame reports a
            // deterministic lastReadError of 0 for a graceful peer close rather
            // than whatever stale value errno happened to hold.
            errno = 0;
            return kXferPeerClose; // EOF (read side); peer closed cleanly
        }
        // got < 0
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;              // re-poll
        if (errno == EPIPE || errno == ECONNRESET)
            return kXferPeerClose;
        return kXferError;
    }
    return kXferOk;
}

} // namespace

bool ControlChannel::writeFrame(const juce::MemoryBlock& body)
{
    std::lock_guard<std::mutex> lk(writeMutex);
    if (impl->fd < 0) return false;
    if (body.getSize() > kMaxControlMessageBytes) return false;

    // Don't watch the stop pipe on writes: a half-written frame would corrupt
    // the stream for the peer. 5 s bounds a stalled reader without pinning the
    // caller until the higher-level request timeout.
    constexpr int kWriteTimeoutMs = 5000;
    uint32_t lenLE = (uint32_t)body.getSize();
    if (transferN(impl->fd, -1, /*isWrite*/true, &lenLE, sizeof(lenLE),
                  kWriteTimeoutMs) != kXferOk)
        return false;
    if (body.getSize() > 0)
    {
        if (transferN(impl->fd, -1, true,
                      const_cast<void*>(body.getData()),
                      body.getSize(), kWriteTimeoutMs) != kXferOk)
            return false;
    }
    return true;
}

bool ControlChannel::readFrame(juce::MemoryBlock& out)
{
    lastReadError = 0;
    lastReadPeerClosed = false;
    if (impl->fd < 0)
    {
        CTL_TRACE("readFrame: fd is closed");
        return false;
    }

    auto classify = [this](int code)
    {
        // Classify the failure for ioLoop's clean-vs-fault decision. A peer
        // EOF/reset (kXferPeerClose) is a clean close; a stop() via the
        // self-pipe (kXferStopped) is ALSO a clean, expected teardown — not an
        // I/O fault — so don't report it as a read-error (matches the Windows
        // CancelIoEx path's intent, and avoids false crash/restart handling if
        // a future change ever delivers this reason). Only a genuine fault
        // (kXferError) maps to read-error. lastReadError is informational.
        lastReadError = (code == kXferStopped) ? 0ul : (unsigned long)errno;
        lastReadPeerClosed = (code == kXferPeerClose || code == kXferStopped);
    };

    // Length prefix: no read timeout (the I/O thread blocks here between
    // frames), but the stop pipe still breaks the wait.
    uint32_t lenLE = 0;
    int rc = transferN(impl->fd, impl->stopPipe[0], /*isWrite*/false,
                       &lenLE, sizeof(lenLE), /*timeoutMs*/ -1);
    if (rc != kXferOk)
    {
        classify(rc);
        CTL_TRACE("readFrame: len read failed rc=%d errno=%lu", rc, lastReadError);
        return false;
    }
    if (lenLE > kMaxControlMessageBytes)
    {
        lastReadError = (unsigned long)EMSGSIZE;
        CTL_TRACE("readFrame: oversized frame len=%lu", (unsigned long)lenLE);
        return false;
    }
    out.setSize(lenLE, false);
    if (lenLE == 0) return true;
    // Finite body timeout once the prefix has arrived (a stalled peer must not
    // wedge the I/O thread). 30 s matches the Windows backend: generous for a
    // multi-MB state-restore frame while bounding the DoS window.
    constexpr int kBodyReadTimeoutMs = 30000;
    rc = transferN(impl->fd, impl->stopPipe[0], false,
                   out.getData(), lenLE, kBodyReadTimeoutMs);
    if (rc != kXferOk)
    {
        classify(rc);
        CTL_TRACE("readFrame: body read failed rc=%d len=%lu errno=%lu",
                  rc, (unsigned long)lenLE, lastReadError);
        return false;
    }
    return true;
}

bool ControlChannel::waitForPeer(juce::String& failReason)
{
    // A socketpair is already connected, so there is nothing to wait for — but
    // honour a stop() that fired between start() and here (mirrors the Windows
    // backend racing the stop event during the connect).
    if (impl->stopPipe[0] >= 0)
    {
        struct pollfd pfd{};
        pfd.fd = impl->stopPipe[0];
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
        {
            failReason = kReasonReadError + " (stopped)";
            return false;
        }
    }
    if (!alive.load(std::memory_order_acquire))
    {
        failReason = kReasonReadError + " (stopped)";
        return false;
    }
    return true;
}

} // namespace slopsmith::sandbox
