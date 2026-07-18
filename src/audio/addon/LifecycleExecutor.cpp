// LifecycleExecutor implementation — see LifecycleExecutor.h for the
// contract and the field evidence that motivated it (guide §12 P0).

#include "LifecycleExecutor.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>

namespace slopsmith::addon {

static std::atomic<std::uint64_t> engineGeneration{1};

std::uint64_t currentEngineGeneration()
{
    return engineGeneration.load(std::memory_order_acquire);
}

std::uint64_t bumpEngineGeneration()
{
    return engineGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
}

// Submission lock: MessageManager::callAsync is itself FIFO, but two
// threads racing between "decide to submit" and "post" could otherwise
// observe an ordering different from the one their callers reasoned about.
// All lifecycle submissions serialize here so queue order == submit order.
static std::mutex submitMutex;

static constexpr int kWatchdogIntervalMs = 15000;

LifecycleOpResult runLifecycleOp(const char* name,
                                 std::function<void()> func,
                                 int maxWaitMs)
{
    const auto stamped = currentEngineGeneration();

    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr)
    {
        fprintf(stderr, "[lifecycle] op '%s': no message pump; refusing\n", name);
        return LifecycleOpResult::neverRan;
    }

    // Already on the message thread (engine init path, macOS inline mode,
    // or a lifecycle op submitting a nested op): dispatch-and-wait on
    // ourselves would deadlock, so run inline.
    //
    // Ordering note (CodeRabbit #120): an inline op runs as part of the
    // CURRENT message-thread turn, i.e. ahead of ops still queued behind
    // it. That is intentional, not a FIFO leak: a nested submission is
    // semantically part of the outer op and must complete inside it, and a
    // message-thread caller can never interleave with a *running* op — the
    // queue drains on this very thread. The FIFO contract is between
    // off-thread submitters, which all take the queued path below.
    if (mm->isThisTheMessageThread())
    {
        if (stamped != currentEngineGeneration())
        {
            fprintf(stderr, "[lifecycle] op '%s': stale generation (inline); skipped\n", name);
            return LifecycleOpResult::stale;
        }
        try { func(); }
        catch (const std::exception& e) {
            fprintf(stderr, "[lifecycle] op '%s' (inline) threw: %s\n", name, e.what());
        }
        catch (...) {
            fprintf(stderr, "[lifecycle] op '%s' (inline) threw (unknown)\n", name);
        }
        return LifecycleOpResult::completed;
    }

    struct Shared {
        juce::WaitableEvent done;
        std::atomic<bool> wasStale{false};
    };
    auto shared = std::make_shared<Shared>();

    bool posted = false;
    {
        std::lock_guard<std::mutex> lock(submitMutex);
        posted = juce::MessageManager::callAsync(
            [func = std::move(func), shared, stamped, name]() mutable {
                if (stamped != currentEngineGeneration())
                {
                    fprintf(stderr, "[lifecycle] op '%s': engine generation moved "
                                    "%llu -> %llu while queued; skipped\n",
                            name,
                            (unsigned long long) stamped,
                            (unsigned long long) currentEngineGeneration());
                    shared->wasStale.store(true, std::memory_order_release);
                }
                else
                {
                    // Always reach the signal below: a throwing op would
                    // otherwise leave its unbounded caller waiting forever
                    // (CodeRabbit #120).
                    try { func(); }
                    catch (const std::exception& e) {
                        fprintf(stderr, "[lifecycle] op '%s' threw: %s\n", name, e.what());
                    }
                    catch (...) {
                        fprintf(stderr, "[lifecycle] op '%s' threw (unknown)\n", name);
                    }
                }
                shared->done.signal();
            });
    }
    if (!posted)
    {
        fprintf(stderr, "[lifecycle] op '%s': message queue refused the post; "
                        "op will not run\n", name);
        return LifecycleOpResult::neverRan;
    }

    int waitedMs = 0;
    while (!shared->done.wait(kWatchdogIntervalMs))
    {
        waitedMs += kWatchdogIntervalMs;

        // Pump death while we wait means the op can never run. Distinguish
        // from a merely blocked pump: the MessageManager singleton is
        // destroyed by stopJuceMessageThread.
        if (juce::MessageManager::getInstanceWithoutCreating() == nullptr)
        {
            fprintf(stderr, "[lifecycle] op '%s': message pump died after %d ms "
                            "wait; op will not run\n", name, waitedMs);
            return LifecycleOpResult::neverRan;
        }

        if (maxWaitMs > 0 && waitedMs >= maxWaitMs)
        {
            fprintf(stderr, "[lifecycle] op '%s': abandoning wait after %d ms "
                            "(bounded caller); op may still run\n", name, waitedMs);
            return LifecycleOpResult::abandonedWait;
        }

        // Watchdog: report, never decide (guide §12 P0 item 5). The usual
        // culprit for a multi-second stall is an audio driver call (ASIO
        // open/reset) blocking the message thread.
        fprintf(stderr, "[lifecycle] watchdog: op '%s' still waiting after %d ms — "
                        "message thread blocked (audio driver call?); continuing "
                        "to wait\n", name, waitedMs);
    }

    return shared->wasStale.load(std::memory_order_acquire)
               ? LifecycleOpResult::stale
               : LifecycleOpResult::completed;
}

} // namespace slopsmith::addon
