#pragma once

#include <juce_core/juce_core.h>

namespace slopsmith::sandbox {

// Process-wide last-chance crash attributor.
//
// The vst-crash-guard sentinel (src/main/vst-crash-guard.ts) only covers the
// brief windows it arms around an in-process load or editor-open. But a plugin
// that creates a top-level window keeps that window for its whole loaded
// lifetime, and the OS can dispatch a message to its WndProc at ANY time (e.g.
// WM_ACTIVATEAPP on an alt-tab) — a fault there arrives via USER32→WndProc with
// no host frame on the stack, outside every armed sentinel window, so it is
// never attributed and the app crash-loops. (See issue #35; diagnosed from dmp
// a06f48e1 / McRocklin Suite.)
//
// installVstCrashAttribution arms a SetUnhandledExceptionFilter that, when a
// fatal fault's faulting instruction lies inside a loaded `.vst3` module,
// stamps `sentinelPath` with {"plugin": <module path>, "op": "native-crash"}
// and then chains to the previously installed top-level filter (Crashpad) so
// the crash dump is still produced and the process dies normally. The next
// launch's initVstCrashGuard() promotes that leftover sentinel into the
// persistent blocklist, routing the offender to the out-of-process sandbox.
//
// This makes the existing dead-man's-pedal cover ANY fatal in-process VST3
// fault, not just the armed load/editor windows. Idempotent: re-calling just
// refreshes the sentinel path. No-op on non-Windows (the POSIX SignalChain
// signal guard already covers the armed call path, and the sandbox is
// Windows-only today).
void installVstCrashAttribution(const juce::String& sentinelPath);

// Restore the previous top-level exception filter and disarm. Safe to call when
// not installed.
void uninstallVstCrashAttribution();

} // namespace slopsmith::sandbox
