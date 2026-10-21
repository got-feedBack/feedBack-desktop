// ThreadJoin.h — race-safe, idempotent std::thread join/detach.
//
// Why this exists: SubprocessHandle::shutdown() and ControlChannel::stop() can
// be invoked concurrently from two threads. SandboxedProcessor::teardown() runs
// them on the control-channel I/O thread (failWith -> onDisconnect -> teardown)
// or on the subprocess watcher thread (onExit -> teardown), while the owner's
// destructor (~SubprocessHandle / ~ControlChannel) runs them on the thread
// destroying the processor — and that destructor path is NOT covered by
// teardown's single-fire latch. Two concurrent join() calls on the same
// std::thread: the winner's join closes the OS thread handle, the loser's
// join then fails with std::system_error(ESRCH, "no such process"), the
// exception escapes the thread entry point, std::terminate fires and the
// whole host process dies. (Observed with a VST3 the sandbox child refused:
// handshake failure -> teardown on the I/O thread racing ~SubprocessHandle on
// the libuv worker thread.)
//
// joinThreadOnce() serialises the join/detach on a per-thread-object state:
//   * exactly one caller performs the join (or the self-detach);
//   * concurrent callers block until it finishes — destructors must not
//     destroy state the thread still captures, so waiting is required (a
//     plain "already joining -> return" flag would reintroduce use-after-free);
//   * a thread joining ITSELF detaches instead (self-join deadlocks) and never
//     waits on the state — the thread being joined may be blocked waiting for
//     this very thread to exit, so waiting would deadlock;
//   * join/detach are wrapped in try/catch: a failed join (e.g. the handle was
//     closed by a concurrent detach) degrades to detach, and even a failed
//     detach is swallowed — shutdown()/stop() must never throw.
//
// Deadlock freedom: the target thread never blocks inside stop()/shutdown()
// (self-join -> detach, or early return when not running), so a caller blocked
// in join() always progresses.
#pragma once

#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

#include "../VSTTrace.h"

namespace slopsmith::sandbox {

struct ThreadJoinState
{
    std::mutex mutex;
    std::condition_variable cv;
    enum State : int { Idle = 0, Joining = 1, Done = 2 };
    State state = Idle;
};

// Joins `t` — or detaches it when called from the thread itself — exactly
// once across all concurrent callers; later callers wait for the join to
// complete. Never throws. `name` is used for trace logging only.
inline void joinThreadOnce(std::thread& t, ThreadJoinState& s, const char* name)
{
    // Self-join check FIRST, before consulting the state: a thread that is
    // itself being joined must never wait for the joiner (that is a deadlock),
    // and its detach must be observable by the joiner (detach closes the OS
    // handle -> the joiner's join() throws -> caught below).
    bool self = false;
    try { self = t.joinable() && (std::this_thread::get_id() == t.get_id()); }
    catch (...) { self = false; }

    std::unique_lock<std::mutex> lk(s.mutex);
    if (self)
    {
        // Detach and, if we were the first caller, publish Done so concurrent
        // destructors can proceed. If another thread is mid-join (state ==
        // Joining), leave the state alone: that thread's join() will throw
        // (we closed the handle) and it will publish Done itself.
        const bool firstCaller = (s.state == ThreadJoinState::Idle);
        if (firstCaller) s.state = ThreadJoinState::Joining;
        lk.unlock();
        try { t.detach(); }
        catch (const std::exception& e)
        {
            VST_TRACE("joinThreadOnce(%s): detach failed: %s", name, e.what());
        }
        catch (...) {}
        if (firstCaller)
        {
            lk.lock();
            s.state = ThreadJoinState::Done;
            lk.unlock();
            s.cv.notify_all();
        }
        return;
    }

    if (s.state == ThreadJoinState::Done)
        return;
    if (s.state == ThreadJoinState::Joining)
    {
        // Another thread is joining right now. Wait for it: returning early
        // would let a destructor free state the joined thread still captures.
        s.cv.wait(lk, [&] { return s.state == ThreadJoinState::Done; });
        return;
    }

    s.state = ThreadJoinState::Joining;
    lk.unlock();

    try
    {
        if (t.joinable())
            t.join();
    }
    catch (const std::exception& e)
    {
        // join() throws std::system_error(no_such_process) when the thread was
        // already joined/detached by a racing self-detach, and
        // resource_deadlock_would_occur if a self-join slipped past the check.
        // Degrade to detach; if even that fails the thread is not joinable
        // anymore and there is nothing left to do. Never propagate.
        VST_TRACE("joinThreadOnce(%s): join failed (%s) — detaching",
                  name, e.what());
        try { if (t.joinable()) t.detach(); } catch (...) {}
    }
    catch (...) {}

    lk.lock();
    s.state = ThreadJoinState::Done;
    lk.unlock();
    s.cv.notify_all();
}

} // namespace slopsmith::sandbox
