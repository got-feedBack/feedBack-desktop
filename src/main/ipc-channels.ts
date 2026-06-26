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
