// ControlChannel — platform-neutral request/reply/event dispatch.
//
// The wire format ([u32 length-LE][utf8 JSON], envelopes in Protocol.cpp), the
// pending-request promise map, and the inbound-frame routing (event vs request
// vs reply) are identical on every OS. Only the transport (named pipe +
// overlapped I/O on Windows, socketpair + poll on POSIX) differs, and that
// lives in ControlChannel_{win,posix}.cpp behind createServerSide /
// connectClientSide / start / stop / waitForPeer / readFrame / writeFrame.

#include "ControlChannelImpl.h"

#include <thread>

// Reuse the existing VST trace logger for diagnostics. Cheap and survives
// crashes thanks to its synchronous flush.
#include "../VSTTrace.h"
#define CTL_TRACE(...) VST_TRACE("[ctrl] " __VA_ARGS__)

namespace slopsmith::sandbox {

const juce::String ControlChannel::kReasonPeerClosed     = "peer-closed";
const juce::String ControlChannel::kReasonReadError      = "read-error";
const juce::String ControlChannel::kReasonProtocolError  = "protocol-error";

ControlChannel::ControlChannel() : impl(std::make_unique<Impl>()) {}

ControlChannel::~ControlChannel()
{
    stop();
}

void ControlChannel::ioLoop()
{
    // Thread entry point: never let an exception escape — an uncaught C++
    // exception on any thread calls std::terminate and kills the whole host
    // process. A throwing dispatch (plugin-originated event, decode failure,
    // promise) fails the channel instead: failWith drains pending requests
    // and fires onDisconnect so the owner tears down cleanly.
    try
    {
        ioLoopBody();
    }
    catch (const std::exception& e)
    {
        CTL_TRACE("ioLoop: uncaught exception (%s) — failing channel", e.what());
        try { failWith(kReasonReadError + " (ioLoop exception)"); } catch (...) {}
    }
    catch (...)
    {
        CTL_TRACE("ioLoop: uncaught unknown exception — failing channel");
        try { failWith(kReasonReadError + " (ioLoop exception)"); } catch (...) {}
    }
}

void ControlChannel::ioLoopBody()
{
    CTL_TRACE("ioLoop entered (isServer=%d)", (int)impl->isServer);
    // Server side: wait for the sandbox to connect (or for stop()). The
    // platform decides what "connect" means; on a clean stop it returns false
    // with a reason already suited to failWith.
    if (impl->isServer)
    {
        juce::String failReason;
        if (!waitForPeer(failReason))
        {
            failWith(failReason);
            return;
        }
    }

    juce::MemoryBlock frame;
    while (alive.load(std::memory_order_acquire))
    {
        if (!readFrame(frame))
        {
            CTL_TRACE("readFrame failed; exiting loop (peerClosed=%d err=%lu)",
                      (int)lastReadPeerClosed, lastReadError);
            // Distinguish a clean peer-side shutdown from an actual I/O fault
            // so the disconnect callback's caller can decide between
            // "expected" and "should restart". readFrame set
            // lastReadPeerClosed per-OS so this stays platform-agnostic.
            failWith(lastReadPeerClosed ? kReasonPeerClosed : kReasonReadError);
            return;
        }
        CTL_TRACE("readFrame got %d bytes", (int)frame.getSize());

        juce::String parseError;
        auto msg = wire::decode(frame.getData(), frame.getSize(), &parseError);
        if (!msg.isObject())
        {
            // %.*s with an explicit length — the frame buffer is not
            // NUL-terminated and can be 0 bytes (no body), so %.32s would
            // read past the end (or dereference null).
            const int previewLen = juce::jmin<int>(32, (int)frame.getSize());
            CTL_TRACE("decode failed: %s; first %d bytes: %.*s",
                      parseError.toRawUTF8(), previewLen,
                      previewLen, (const char*)frame.getData());
            failWith(kReasonProtocolError + ": " + parseError);
            return;
        }

        // Reject frames missing or mismatching the protocol version — better
        // to fail fast on host/sandbox skew than to keep going and misparse a
        // payload that doesn't match the schema we expect.
        const int incomingVersion = (int)msg.getProperty("v", -1);
        if (incomingVersion != (int)kProtocolVersion)
        {
            CTL_TRACE("protocol version mismatch: got=%d expected=%d",
                      incomingVersion, (int)kProtocolVersion);
            failWith(kReasonProtocolError + ": version mismatch (got "
                     + juce::String(incomingVersion) + ", expected "
                     + juce::String((int)kProtocolVersion) + ")");
            return;
        }

        // Reply ({id, ok, result/error}) vs event ({event, data}) vs request
        // ({id, op, args}). Dispatch by structure.
        if (msg.hasProperty("event"))
        {
            CTL_TRACE("event: %s", msg["event"].toString().toRawUTF8());
            if (onEvent)
                onEvent(msg["event"].toString(), msg["data"]);
            continue;
        }
        if (msg.hasProperty("op"))
        {
            const int id = (int)msg.getProperty("id", -1);
            if (requestHandler)
            {
                requestHandler(id, msg["op"].toString(), msg["args"]);
            }
            else if (id >= 0)
            {
                // No handler installed (host side never accepts inbound
                // requests). Reply with an explicit error so a misbehaving
                // or forged peer can't pin our request() with a 10 s wait.
                sendReply(id, false, {}, "no request handler installed");
            }
            continue;
        }
        // Reply path
        int id = (int)msg.getProperty("id", -1);
        std::shared_ptr<Pending> pendingEntry;
        {
            std::lock_guard<std::mutex> lk(pendingMutex);
            auto it = pending.find(id);
            if (it != pending.end())
            {
                pendingEntry = it->second;
                pending.erase(it);
            }
        }
        if (pendingEntry)
        {
            bool ok = (bool)msg.getProperty("ok", false);
            juce::DynamicObject::Ptr replyObj(new juce::DynamicObject());
            replyObj->setProperty("ok", ok);
            replyObj->setProperty("result", msg["result"]);
            replyObj->setProperty("error", msg["error"]);
            try { pendingEntry->promise.set_value(juce::var(replyObj.get())); }
            catch (const std::future_error&) {}
        }
    }
}

void ControlChannel::failWith(const juce::String& reason)
{
    if (!alive.exchange(false, std::memory_order_acq_rel)) return;

    // Drain internal state BEFORE invoking the callback. The disconnect
    // handler is allowed to tear down higher-level owners that destroy this
    // ControlChannel (typical teardown chain: SandboxedProcessor::teardown
    // → ControlChannel::stop → ~ControlChannel), so any member access after
    // the callback returns would be use-after-free.
    {
        std::lock_guard<std::mutex> lk(pendingMutex);
        for (auto& [id, p] : pending)
        {
            try { p->promise.set_value({}); } catch (...) {}
        }
        pending.clear();
    }

    auto cb = std::move(onDisconnect);
    if (cb) cb(reason);
}

juce::var ControlChannel::request(const char* op, const juce::var& args,
                                   int timeoutMs, juce::String* errorOut)
{
    if (!alive.load(std::memory_order_acquire))
    {
        if (errorOut) *errorOut = "channel not alive";
        return {};
    }
    int id = nextRequestId.fetch_add(1, std::memory_order_relaxed);

    auto entry = std::make_shared<Pending>();
    auto fut = entry->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(pendingMutex);
        pending[id] = entry;
    }

    auto frame = wire::encode(wire::makeRequest(id, op, args));
    if (!writeFrame(frame))
    {
        std::lock_guard<std::mutex> lk(pendingMutex);
        pending.erase(id);
        if (errorOut) *errorOut = "write failed";
        return {};
    }

    if (fut.wait_for(std::chrono::milliseconds(timeoutMs))
        == std::future_status::timeout)
    {
        std::lock_guard<std::mutex> lk(pendingMutex);
        pending.erase(id);
        if (errorOut) *errorOut = "timeout";
        return {};
    }

    auto reply = fut.get();
    // stop() / failWith() resolve in-flight requests with an undefined `var`
    // so callers don't hang. Detect that case explicitly — otherwise
    // getProperty(...) returns defaults and the caller sees an empty
    // errorOut even though the real cause was disconnect/cancellation.
    if (!reply.isObject())
    {
        if (errorOut) *errorOut = "control channel disconnected";
        return {};
    }
    if (!(bool)reply.getProperty("ok", false))
    {
        if (errorOut) *errorOut = reply.getProperty("error", "").toString();
        return {};
    }
    return reply["result"];
}

bool ControlChannel::postNoReply(const char* op, const juce::var& args)
{
    if (!alive.load(std::memory_order_acquire)) return false;
    auto frame = wire::encode(wire::makeRequest(-1, op, args));
    return writeFrame(frame);
}

bool ControlChannel::sendReply(int requestId, bool ok, const juce::var& result,
                                const juce::String& errorMessage)
{
    auto frame = wire::encode(wire::makeReply(requestId, ok, result, errorMessage));
    return writeFrame(frame);
}

bool ControlChannel::sendEvent(const char* eventName, const juce::var& data)
{
    auto frame = wire::encode(wire::makeEvent(eventName, data));
    return writeFrame(frame);
}

void ControlChannel::setRequestHandler(RequestHandler handler)
{
    // The I/O thread reads requestHandler unsynchronized — assignments after
    // start() would race. Header documents "MUST be called BEFORE start()";
    // assert it so a future regression (e.g. wiring a handler from a ready
    // callback) fails loudly in debug builds rather than silently racing.
    jassert(!ioThread.joinable() && !alive.load(std::memory_order_acquire));
    requestHandler = std::move(handler);
}

} // namespace slopsmith::sandbox
