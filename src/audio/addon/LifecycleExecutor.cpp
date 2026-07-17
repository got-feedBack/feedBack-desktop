// LifecycleExecutor implementation — see LifecycleExecutor.h for the
// contract and the field evidence that motivated it (guide §12 P0).

#include "LifecycleExecutor.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif

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

    // Already on the message thread (engine init path, macOS inline mode):
    // dispatch-and-wait on ourselves would deadlock. Run inline — we are by
    // definition serialized with every queued op.
    if (mm->isThisTheMessageThread())
    {
        if (stamped != currentEngineGeneration())
        {
            fprintf(stderr, "[lifecycle] op '%s': stale generation (inline); skipped\n", name);
            return LifecycleOpResult::stale;
        }
        func();
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
                    func();
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

void pinPluginModuleForever(const char* fileOrIdentifierUtf8)
{
#if JUCE_WINDOWS
    // JUCE loads the inner <bundle>/Contents/x86_64-win/<name>.vst3 (or a
    // flat .vst3/.dll); either way the loaded module's base name is the
    // path's final component. Pin by that name so the loader never unloads
    // it, even when JUCE's module refcount hits zero.
    const juce::String path = juce::String::fromUTF8(fileOrIdentifierUtf8);
    const juce::String base = path.replaceCharacter('\\', '/')
                                  .fromLastOccurrenceOf("/", false, false);
    if (base.isEmpty())
        return;

    HMODULE h = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN,
                           base.toWideCharPointer(), &h))
    {
        fprintf(stderr, "[lifecycle] pinned plugin module '%s' for process "
                        "lifetime\n", base.toRawUTF8());
    }
    // Not currently loaded under that name (sandboxed plugin, or the inner
    // file is named differently): nothing to pin — the sandbox child owns
    // its own modules, and an in-process module we can't resolve here was
    // loaded under some other base name and will be pinned on a later load
    // if it ever resolves. Silent by design; this is a belt on JUCE's
    // refcount braces, not a correctness gate.
#else
    (void) fileOrIdentifierUtf8;
#endif
}

} // namespace slopsmith::addon
