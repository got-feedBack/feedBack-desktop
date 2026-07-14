#pragma once

// EditorWindows — in-process plugin editor windows + the open/close bindings
// and the Windows sandbox-promotion flow (TLC plan phase 7 / §3.4). Moved
// verbatim from NodeAddon.cpp. Owns the slotId→window map (message-thread
// only) and the teardown helpers every chain-clearing path must run BEFORE
// freeing slot processors (use-after-free; feedBack-desktop#56).

#include <napi.h>

namespace slopsmith::addon {

// Inline teardown: destroys every editor window. Caller MUST already be on
// the message thread (the window map holds JUCE GUI objects). doShutdown's
// UI teardown hook points here.
void destroyAllPluginEditorWindowsOnMessageThread();

// Tears down the in-process editor windows so they are destroyed before the
// caller frees the processors those editors point at. Safe from the Node
// thread (posts to the message thread and blocks, bounded) or the message
// thread itself (inline). Clearing an empty map is cheap.
// Returns false when teardown did NOT complete (post refused or the bounded
// wait timed out) — the caller must not free chain processors in that case
// (#56 use-after-free).
bool closeAllPluginEditorWindows();

// N-API bindings (registered by NodeAddon's export table).
Napi::Value OpenPluginEditor(const Napi::CallbackInfo& info);
Napi::Value ClosePluginEditor(const Napi::CallbackInfo& info);

} // namespace slopsmith::addon
