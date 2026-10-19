#pragma once

// GuardedAsyncWorker — exception guard for N-API AsyncWorker.
//
// The addon is built with NAPI_DISABLE_CPP_EXCEPTIONS (src/audio/CMakeLists.txt),
// so node-addon-api performs NO exception handling around AsyncWorker::Execute().
// A C++ exception escaping Execute() (a throwing VST3 during load, a failed
// allocation, ...) would call std::terminate and kill the whole host process —
// and with /EHa even a plugin's structured exception (access violation) reaches
// here as a catchable exception. Workers deriving from GuardedAsyncWorker get a
// final Execute() that converts any escaping exception into
// SetError -> OnError -> a rejected JS promise. The app keeps running and the
// caller sees a normal error — the same contract a DAW gives for a misbehaving
// plugin.

#include <napi.h>

#include <exception>
#include <string>

namespace slopsmith::addon {

template <typename Derived>
class GuardedAsyncWorker : public Napi::AsyncWorker
{
public:
    using Napi::AsyncWorker::AsyncWorker;

    void Execute() override final
    {
        try
        {
            static_cast<Derived*>(this)->ExecuteImpl();
        }
        catch (const std::exception& e)
        {
            SetError(std::string("native worker exception: ") + e.what());
        }
        catch (...)
        {
            SetError("unknown native worker exception");
        }
    }
};

} // namespace slopsmith::addon
