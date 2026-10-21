// ControlChannel — request/response + event messaging between the host
// (Slopsmith Desktop) and a sandbox subprocess.
//
// Transport: Windows named pipe in byte mode (PIPE_TYPE_BYTE |
// PIPE_READMODE_BYTE) with an explicit `[u32 length-LE][body]` framing
// layer. PIPE_TYPE_MESSAGE was tried first but dropped because the
// sandbox's `ready` frame wasn't being delivered to the host I/O thread
// reliably — see commit 2cb9ae9. Posix transport TBD when the macOS /
// Linux sandbox PRs land.
//
// Threading model: one I/O thread inside the channel reads frames and dispatches
// them to the event callback or to the matching pending request future. Callers
// invoke `request()` from arbitrary threads.

#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "Protocol.h"

namespace slopsmith::sandbox {

class ControlChannel
{
public:
    // Async callback fired for each sandbox-originated event. Invoked from the
    // channel's internal I/O thread; the callback must not block.
    using EventCallback = std::function<void(const juce::String& event,
                                             const juce::var& data)>;

    // Sentinel reason strings passed to the disconnect callback.
    static const juce::String kReasonPeerClosed;
    static const juce::String kReasonReadError;
    static const juce::String kReasonProtocolError;

    ControlChannel();
    ~ControlChannel();

    // Host-side: create a uniquely-named pipe in CONNECT (server) mode and
    // return the pipe name. The sandbox subprocess will connect to it shortly
    // after spawn.
    bool createServerSide(juce::String& pipeNameOut, juce::String& errorOut);

    // Sandbox-side: connect to a pipe created by the host. Windows only —
    // the named pipe is re-opened by name. POSIX uses connectClientSideFd
    // (the control transport is a fd-passed socketpair, not a named object).
    bool connectClientSide(const juce::String& pipeName, juce::String& errorOut);

   #if ! JUCE_WINDOWS
    // POSIX sandbox-side connect: adopt the inherited socketpair end (the fd
    // the spawner dup2()'d into us, or — in an in-process loopback —
    // server.sandboxFd()). Takes ownership of `fd`. The Windows
    // connectClientSide(name) overload is unavailable on POSIX.
    bool connectClientSideFd(int fd, juce::String& errorOut);

    // POSIX host-side: the sandbox's end of the socketpair created by
    // createServerSide. The caller owns it — dup2() it into the child (then
    // close the copy) or hand it to an in-process connectClientSideFd.
    // Returns -1 before createServerSide or on a non-server channel.
    int sandboxFd() const noexcept;

    // POSIX host-side: close our copy of the sandbox's socketpair end after a
    // real spawn (the spawner has dup2()'d it into the child). REQUIRED — if
    // the host keeps this open, the host end never observes EOF when the child
    // dies, so crash detection never fires. No-op for the in-process loopback
    // (connectClientSideFd already adopted the fd).
    void closeSandboxFd() noexcept;
   #endif

    // Start the background I/O thread. Must be called after either
    // createServerSide() or connectClientSide(). Returns false on error;
    // callers can read getLastStartError() for a diagnostic string.
    bool start(EventCallback onEvent,
               std::function<void(const juce::String& reason)> onDisconnect);

    // Diagnostic reason for the most recent start() failure (re-start
    // attempted, no pipe, CreateEventW failure). Empty if start succeeded
    // or has not been called.
    juce::String getLastStartError() const { return lastStartError; }

    void stop();
    bool isAlive() const noexcept { return alive.load(std::memory_order_acquire); }

    // Synchronous request/response. Returns the parsed result `juce::var`, or
    // an undefined `var` on timeout/error (with the reason in `errorOut`).
    juce::var request(const char* op, const juce::var& args,
                      int timeoutMs, juce::String* errorOut = nullptr);

    // Fire-and-forget: no reply expected. Used for high-frequency messages
    // like MIDI events and parameter automation.
    bool postNoReply(const char* op, const juce::var& args);

    // Sandbox-side helpers: send a reply to the host's request, or originate
    // an event.
    bool sendReply(int requestId, bool ok, const juce::var& result,
                   const juce::String& errorMessage = {});
    bool sendEvent(const char* eventName, const juce::var& data);

    // Sandbox-side: when the channel parses an inbound request, the consumer
    // installs a request handler. MUST be called BEFORE start() — the I/O
    // thread reads `requestHandler` on every inbound request, and the
    // member is intentionally not synchronised. Installing after start()
    // races the read.
    using RequestHandler =
        std::function<void(int requestId, const juce::String& op,
                           const juce::var& args)>;
    void setRequestHandler(RequestHandler handler);

private:
    struct Pending
    {
        std::promise<juce::var> promise;
    };

    bool writeFrame(const juce::MemoryBlock& body);
    bool readFrame(juce::MemoryBlock& out);
    // Server side: block until the sandbox connects (Windows: ConnectNamedPipe;
    // POSIX: socketpair is already connected, so this just races the stop
    // signal). Returns false if stop() fired first; `failReason` is set for
    // failWith. Platform-defined.
    bool waitForPeer(juce::String& failReason);
    void ioLoop();
    // ioLoop() body — the frame dispatch loop. Split out so ioLoop() can wrap
    // it in a catch-all: an exception escaping a thread entry point calls
    // std::terminate and kills the whole host process.
    void ioLoopBody();
    void failWith(const juce::String& reason);

    // Set by readFrame() before it returns false so ioLoop can classify the
    // disconnect. `lastReadError` is the raw OS error (Win32 GetLastError /
    // POSIX errno) for diagnostics; `lastReadPeerClosed` is the
    // platform-agnostic verdict ioLoop acts on (a clean peer-side close /
    // EOF → kReasonPeerClosed, anything else → kReasonReadError) so the shared
    // dispatch loop never has to know per-OS error codes. Only the I/O thread
    // writes or reads these fields.
    unsigned long lastReadError = 0;
    bool lastReadPeerClosed = false;

    struct Impl;
    std::unique_ptr<Impl> impl; // OS-specific handle wrapper

    std::atomic<bool> alive{false};
    std::atomic<int> nextRequestId{1};

    EventCallback onEvent;
    std::function<void(const juce::String& reason)> onDisconnect;
    RequestHandler requestHandler;

    std::mutex pendingMutex;
    std::unordered_map<int, std::shared_ptr<Pending>> pending;

    std::mutex writeMutex;     // serialises outbound writes
    std::thread ioThread;
    juce::String lastStartError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ControlChannel)
};

} // namespace slopsmith::sandbox
