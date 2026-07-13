#include "ChainOps.h"

#include <atomic>

namespace slopsmith::addon {

std::mutex& chainMutationMutex()
{
    static std::mutex m;
    return m;
}

static std::atomic<uint64_t> chainGeneration{0};

uint64_t bumpChainGeneration()
{
    return chainGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
}

uint64_t currentChainGeneration()
{
    return chainGeneration.load(std::memory_order_acquire);
}

} // namespace slopsmith::addon
