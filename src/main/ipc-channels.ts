// Central registry of IPC channel names shared between the main process and
// preload scripts. Import this module in both sides so a rename never drifts.

export const IPC_STARTUP_STATUS = 'startup:status' as const;
export const IPC_STARTUP_GET_STATUS = 'startup:getStatus' as const;
export const IPC_STARTUP_REQUEST_STATUS = 'startup:requestStatus' as const;

// Auto-update (Velopack). The renderer (Settings panel + restart banner) reads
// status, switches release channel, kicks a manual check, and applies a
// downloaded update.
export const IPC_UPDATE_GET_STATUS = 'update:getStatus' as const;
export const IPC_UPDATE_SET_CHANNEL = 'update:setChannel' as const;
export const IPC_UPDATE_CHECK_NOW = 'update:checkNow' as const;
export const IPC_UPDATE_APPLY = 'update:apply' as const;

// One-way push events the main side broadcasts to every BrowserWindow via
// webContents.send (not ipcMain.handle channels). Registered here so the
// update-manager broadcaster and the preload listeners can't drift.
export const IPC_UPDATE_EVENT_AVAILABLE = 'update:available' as const;
export const IPC_UPDATE_EVENT_DOWNLOADED = 'update:downloaded' as const;

// Config maintenance — the in-app "Reset / repair configuration" action. The
// Settings panel reads the enumerated per-OS paths, runs a granular reset, and
// asks the main process to relaunch. Replaces the manual "delete the config
// folder" instruction.
export const IPC_MAINTENANCE_GET_PATHS = 'maintenance:getPaths' as const;
export const IPC_MAINTENANCE_RESET = 'maintenance:reset' as const;
export const IPC_MAINTENANCE_RESTART = 'maintenance:restart' as const;

// Screen wake lock. The renderer (slopsmith core app.js) asks the main process
// to keep the display awake while a song plays — embedded Chromium does not
// honour the renderer's navigator.wakeLock reliably, so we drive Electron's
// powerSaveBlocker here instead. See got-feedback/feedback#686.
export const IPC_POWER_SET_SCREEN_AWAKE = 'power:setScreenAwake' as const;

// Detachable panes (feedBack core's window.feedBack.panes). The renderer owns
// the truth — which panes exist, which are open, and what goes in them; the main
// process owns the OS surfaces: one BrowserWindow per popped-out pane, their
// remembered geometry, and the system tray.
//
// The pane window loads <renderer origin>/pane, so it shares the renderer's
// BroadcastChannel scope and talks to the app over the same channel a browser
// pop-out would. Main never sees a pane's contents.
export const IPC_PANE_OPEN = 'pane:open' as const;
export const IPC_PANE_CLOSE = 'pane:close' as const;
export const IPC_PANE_FOCUS = 'pane:focus' as const;
export const IPC_PANE_SET_ALWAYS_ON_TOP = 'pane:setAlwaysOnTop' as const;
// The renderer pushes its pane registry up whenever it changes, so the tray menu
// can list panes it otherwise knows nothing about.
export const IPC_PANE_SYNC = 'pane:sync' as const;

// One-way pushes, main → renderer.
// A pane window the user closed (or that crashed): the renderer must close the
// pane so the chip's hidden dialog comes back.
export const IPC_PANE_EVENT_CLOSED = 'pane:closed' as const;
// The tray asked for a pane to be opened or closed. The renderer decides what
// that means and calls back through pane:open / pane:close.
export const IPC_PANE_EVENT_TOGGLE = 'pane:toggle' as const;
