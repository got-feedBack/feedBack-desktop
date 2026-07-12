// Pane pop-out windows.
//
// feedBack core has a pane system (window.feedBack.panes): live UI — a mixer, a
// camera rig, a readout — authored once and hostable anywhere. In a plain
// browser it pops out via window.open(). Here it gets a real BrowserWindow, with
// remembered geometry, always-on-top, and a system tray (pane-tray.ts).
//
// Division of labour: THE RENDERER OWNS THE TRUTH. It knows which panes exist,
// which are open, and what is in them. This module owns OS surfaces only —
// windows and their geometry. It never looks inside a pane.
//
// The window loads `<renderer origin>/pane?...`, which matters for one specific
// reason: a pane is fed over BroadcastChannel, and BroadcastChannel only reaches
// windows in the same Chromium instance and origin. Push the URL to the system
// browser and the pane opens looking perfect and never updates again. That is
// also why main.ts's setWindowOpenHandler answers same-origin URLs with
// `action: 'allow'` rather than `deny` + shell.openExternal.

import { BrowserWindow, ipcMain, screen } from 'electron';
import {
    IPC_PANE_OPEN,
    IPC_PANE_CLOSE,
    IPC_PANE_FOCUS,
    IPC_PANE_SET_ALWAYS_ON_TOP,
    IPC_PANE_SYNC,
    IPC_PANE_EVENT_CLOSED,
} from './ipc-channels';
import { sanitizeWindowBounds, type WindowSizing } from './window-bounds';
import { getDesktopConfig, setDesktopConfig, type SavedPaneWindow } from './soundfont-manager';
import { setTrayPanes, type TrayPane } from './pane-tray';

// A pane window is small by nature. The main window's 800x600 floor would
// inflate one threefold, which is why sanitizeWindowBounds takes sizing now.
const PANE_SIZING: WindowSizing = {
    minWidth: 260,
    minHeight: 200,
    defaultWidth: 380,
    defaultHeight: 560,
};

interface PaneOpenRequest {
    paneId: string;
    url: string;
    title: string;
    width?: number;
    height?: number;
}

const windows = new Map<string, BrowserWindow>();
let getMainWindow: () => BrowserWindow | null = () => null;
let isRendererOrigin: (url: string) => boolean = () => false;

function notifyRenderer(channel: string, payload: unknown): void {
    const win = getMainWindow();
    if (win && !win.isDestroyed()) win.webContents.send(channel, payload);
}

// ── Geometry ────────────────────────────────────────────────────────────────

function savedFor(paneId: string): SavedPaneWindow {
    return getDesktopConfig().paneWindows?.[paneId] ?? {};
}

function persist(paneId: string, patch: SavedPaneWindow): void {
    try {
        // setDesktopConfig merges shallowly, so paneWindows must be
        // read-modify-written or one pane's save would drop every other pane's.
        const all = { ...(getDesktopConfig().paneWindows ?? {}) };
        all[paneId] = { ...(all[paneId] ?? {}), ...patch };
        setDesktopConfig({ paneWindows: all });
    } catch (err) {
        console.warn(`[panes] failed to persist geometry for ${paneId}:`, err);
    }
}

// ── Windows ─────────────────────────────────────────────────────────────────

function openPane(req: PaneOpenRequest, webPreferences: Electron.WebPreferences): boolean {
    // IPC is untyped at runtime. Validate before handing anything to
    // BrowserWindow — and above all, refuse a URL that is not the renderer's own
    // origin, or we would be opening arbitrary web content with the full preload
    // bridge attached.
    if (typeof req?.paneId !== 'string' || !req.paneId) return false;
    if (typeof req?.url !== 'string' || !isRendererOrigin(req.url)) {
        console.warn(`[panes] refusing to open a pane at a non-renderer origin: ${String(req?.url)}`);
        return false;
    }

    const existing = windows.get(req.paneId);
    if (existing && !existing.isDestroyed()) {
        // Already open: show it rather than opening a second copy. A pane that is
        // hidden in the tray comes back here.
        if (!existing.isVisible()) existing.show();
        existing.focus();
        return true;
    }

    const saved = savedFor(req.paneId);
    const restored = sanitizeWindowBounds(
        saved.bounds,
        screen.getAllDisplays().map((d) => d.workArea),
        {
            ...PANE_SIZING,
            // The pane's own declared size is the fallback when it has never been
            // opened before; the saved bounds win once it has.
            defaultWidth: req.width ?? PANE_SIZING.defaultWidth,
            defaultHeight: req.height ?? PANE_SIZING.defaultHeight,
        },
    );

    const win = new BrowserWindow({
        x: restored.x,
        y: restored.y,
        width: restored.width,
        height: restored.height,
        minWidth: PANE_SIZING.minWidth,
        minHeight: PANE_SIZING.minHeight,
        title: req.title || 'fee[dB]ack',
        backgroundColor: '#0f172a',
        alwaysOnTop: saved.alwaysOnTop === true,
        // A pane is a companion to the app, not an entry to it: keep it off the
        // taskbar so it never masquerades as a second fee[dB]ack.
        skipTaskbar: true,
        webPreferences,
    });

    windows.set(req.paneId, win);

    // Persist on move/resize, not just on close — a pane window can outlive the
    // app in a crash, and the whole point of remembering geometry is that the
    // user never has to place it twice.
    const save = (): void => {
        if (win.isDestroyed()) return;
        persist(req.paneId, { bounds: { ...win.getNormalBounds(), maximized: false } });
    };
    win.on('moved', save);
    win.on('resized', save);

    // Minimize sends the pane to the tray, not to the taskbar. Panes are small
    // and numerous; a taskbar full of them is noise, and the tray already lists
    // them. Electron's 'minimize' is not cancellable (the listener takes no
    // event), so we hide immediately after rather than preventing it — and since
    // the window is skipTaskbar there is no minimize animation to see.
    win.on('minimize', () => {
        win.hide();
        refreshTray();
    });

    win.on('closed', () => {
        windows.delete(req.paneId);
        // Tell the renderer, or the pane stays "open" in its registry forever —
        // and the dialog the pop-out chip hid never comes back, leaving the user
        // with no way to reach their own UI.
        notifyRenderer(IPC_PANE_EVENT_CLOSED, { paneId: req.paneId });
        refreshTray();
    });

    void win.loadURL(req.url);
    refreshTray();
    return true;
}

function closePane(paneId: string): void {
    const win = windows.get(paneId);
    windows.delete(paneId);
    if (win && !win.isDestroyed()) win.destroy();
}

export function closeAllPanes(): void {
    // Called when the main window goes. A pane window can never be fed again
    // without it — and worse, a HIDDEN pane window is still an open window, so
    // leaving one behind would keep `window-all-closed` from ever firing and the
    // app would linger as an invisible process.
    Array.from(windows.keys()).forEach(closePane);
}

// ── Tray ────────────────────────────────────────────────────────────────────

// The renderer's last known pane registry. The tray menu is built from this plus
// the live window state, so the tray can offer panes it knows nothing else about.
let lastSync: TrayPane[] = [];

function refreshTray(): void {
    setTrayPanes(lastSync.map((p) => {
        const win = windows.get(p.id);
        return {
            ...p,
            // "open" from the tray's point of view means "has a visible window".
            // A pane docked inside the main window is not something the tray can
            // usefully show or hide.
            open: !!win && !win.isDestroyed() && win.isVisible(),
        };
    }));
}

// ── Wiring ──────────────────────────────────────────────────────────────────

export function initPaneHosts(deps: {
    getMainWindow: () => BrowserWindow | null;
    isRendererOrigin: (url: string) => boolean;
    webPreferences: Electron.WebPreferences;
}): void {
    getMainWindow = deps.getMainWindow;
    isRendererOrigin = deps.isRendererOrigin;

    ipcMain.handle(IPC_PANE_OPEN, (_event, req: unknown) => openPane(req as PaneOpenRequest, deps.webPreferences));

    ipcMain.handle(IPC_PANE_CLOSE, (_event, paneId: unknown) => {
        if (typeof paneId !== 'string') return false;
        closePane(paneId);
        refreshTray();
        return true;
    });

    ipcMain.handle(IPC_PANE_FOCUS, (_event, paneId: unknown) => {
        if (typeof paneId !== 'string') return false;
        const win = windows.get(paneId);
        if (!win || win.isDestroyed()) return false;
        if (!win.isVisible()) win.show();
        win.focus();
        refreshTray();
        return true;
    });

    ipcMain.handle(IPC_PANE_SET_ALWAYS_ON_TOP, (_event, paneId: unknown, value: unknown) => {
        if (typeof paneId !== 'string') return false;
        const on = value === true;
        const win = windows.get(paneId);
        if (win && !win.isDestroyed()) win.setAlwaysOnTop(on);
        persist(paneId, { alwaysOnTop: on });
        return true;
    });

    // The renderer pushes its registry whenever a pane is registered, opened or
    // closed. Fire-and-forget: the tray is a view of the renderer's truth, never
    // a second copy of it.
    ipcMain.on(IPC_PANE_SYNC, (_event, panes: unknown) => {
        lastSync = Array.isArray(panes)
            ? panes.filter((p): p is TrayPane =>
                !!p && typeof p.id === 'string' && typeof p.title === 'string')
            : [];
        refreshTray();
    });
}

// Exposed for the tray: show/hide a pane window we already have.
export function togglePaneWindow(paneId: string): boolean {
    const win = windows.get(paneId);
    if (!win || win.isDestroyed()) return false;   // not open → the renderer must open it
    if (win.isVisible()) win.hide(); else win.show();
    refreshTray();
    return true;
}

export function showAllPaneWindows(): void {
    windows.forEach((win) => { if (!win.isDestroyed() && !win.isVisible()) win.show(); });
    refreshTray();
}

export function hideAllPaneWindows(): void {
    windows.forEach((win) => { if (!win.isDestroyed() && win.isVisible()) win.hide(); });
    refreshTray();
}

export function hasPaneWindow(paneId: string): boolean {
    const win = windows.get(paneId);
    return !!win && !win.isDestroyed();
}
