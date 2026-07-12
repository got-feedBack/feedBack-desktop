// Pane pop-out windows.
//
// feedBack core has a pane system (window.feedBack.panes): a plugin's panel — a
// mixer, a camera rig, a readout — popped out of the app into its own window and
// left there. Here it gets the desktop treatment: remembered geometry, off the
// taskbar, minimize-to-tray, and a tray menu that lists every pane (pane-tray.ts).
//
// READ THIS BEFORE "TIDYING UP" THE WINDOW CREATION.
//
// We do NOT create these windows. The renderer opens them with window.open(), and
// Electron's setWindowOpenHandler (main.ts) turns that into a real BrowserWindow
// for us. That is not an accident and it is not laziness:
//
//   The pane's element is MOVED into the pop-out window — the actual DOM node,
//   adopted across same-origin documents, so it keeps its listeners and its
//   closures and goes on running the plugin's own code. To adopt it, the renderer
//   needs a handle on the new window's document. A window WE created in the main
//   process gives it no such handle. Create the window here and the whole feature
//   collapses back into "reimplement the panel and sync it over IPC".
//
// So the renderer opens the window, names its frame `fbpane-<paneId>`, and we
// recognise it in main.ts's did-create-window and attach the OS behaviour. The
// renderer keeps the DOM link; we supply the window manners.

import { BrowserWindow, ipcMain, screen } from 'electron';
import { IPC_PANE_SYNC } from './ipc-channels';
import { sanitizeWindowBounds, type WindowSizing } from './window-bounds';
import { getDesktopConfig, setDesktopConfig, type SavedPaneWindow } from './soundfont-manager';
import { setTrayPanes, type TrayPane } from './pane-tray';

// The renderer names the frame `fbpane-<paneId>`. Keep in sync with
// static/panes/pane-window-host.js.
const FRAME_PREFIX = 'fbpane-';

// A pane window is small by nature. The main window's 800x600 floor would inflate
// one threefold, which is why sanitizeWindowBounds takes sizing now.
const PANE_SIZING: WindowSizing = {
    minWidth: 240,
    minHeight: 180,
    defaultWidth: 380,
    defaultHeight: 560,
};

const windows = new Map<string, BrowserWindow>();
let getMainWindow: () => BrowserWindow | null = () => null;

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

// ── Adoption ────────────────────────────────────────────────────────────────

export function paneIdFromFrameName(frameName: string): string | null {
    if (!frameName || !frameName.startsWith(FRAME_PREFIX)) return null;
    const id = frameName.slice(FRAME_PREFIX.length);
    return id ? id : null;
}

// Called from main.ts's did-create-window when the renderer pops a pane out.
export function adoptPaneWindow(win: BrowserWindow, paneId: string): void {
    windows.set(paneId, win);

    const saved = savedFor(paneId);
    const restored = sanitizeWindowBounds(
        saved.bounds,
        screen.getAllDisplays().map((d) => d.workArea),
        // The size window.open() asked for is the fallback for a pane that has
        // never been opened before; the saved bounds win once it has.
        { ...PANE_SIZING, defaultWidth: win.getBounds().width, defaultHeight: win.getBounds().height },
    );
    if (restored.x !== undefined && restored.y !== undefined) {
        win.setBounds({ x: restored.x, y: restored.y, width: restored.width, height: restored.height });
    } else {
        win.setSize(restored.width, restored.height);
    }
    win.setMinimumSize(PANE_SIZING.minWidth, PANE_SIZING.minHeight);
    if (saved.alwaysOnTop === true) win.setAlwaysOnTop(true);

    // A pane is a companion to the app, not an entry to it: keep it off the taskbar
    // so it never masquerades as a second fee[dB]ack.
    win.setSkipTaskbar(true);

    // Persist on move/resize, not only on close — a pane window can outlive the app
    // in a crash, and the whole point of remembering geometry is that you never
    // place it twice.
    const save = (): void => {
        if (win.isDestroyed()) return;
        persist(paneId, { bounds: { ...win.getNormalBounds(), maximized: false } });
    };
    win.on('moved', save);
    win.on('resized', save);

    // Minimize sends a pane to the tray, not the taskbar. Panes are small and
    // numerous; a taskbar full of them is noise, and the tray already lists them.
    // Electron's 'minimize' is not cancellable here (the listener takes no event),
    // so we hide right after rather than preventing it — and the window is
    // skipTaskbar, so there is no animation to see.
    win.on('minimize', () => {
        win.hide();
        refreshTray();
    });

    win.on('closed', () => {
        windows.delete(paneId);
        refreshTray();
        // No IPC needed to tell the renderer: it opened this window itself and holds
        // the WindowProxy, so it already knows — and it has to, because its element
        // is inside and must be brought home.
    });

    refreshTray();
}

export function closeAllPanes(): void {
    // Called when the main window goes. A pane window holds a DOM node belonging to
    // the main window's document — with the main window gone there is nothing left
    // to dock it back into. And worse: a pane HIDDEN in the tray is still an open
    // window, so leaving one behind would stop `window-all-closed` from ever firing
    // and the app would linger as an invisible process.
    Array.from(windows.values()).forEach((win) => { if (!win.isDestroyed()) win.destroy(); });
    windows.clear();
}

// ── Tray ────────────────────────────────────────────────────────────────────

// The renderer's last known pane registry. The tray menu is a VIEW of it, never a
// second copy — main has no idea what a pane contains, and does not need one.
let lastSync: TrayPane[] = [];

function refreshTray(): void {
    setTrayPanes(lastSync.map((p) => {
        const win = windows.get(p.id);
        return {
            ...p,
            // "open", to the tray, means "has a visible window". A pane docked inside
            // the main window is not something the tray can usefully show or hide.
            open: !!win && !win.isDestroyed() && win.isVisible(),
        };
    }));
}

export function togglePaneWindow(paneId: string): boolean {
    const win = windows.get(paneId);
    if (!win || win.isDestroyed()) return false;   // not open → only the renderer can open it
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

// ── Wiring ──────────────────────────────────────────────────────────────────

export function initPaneHosts(deps: { getMainWindow: () => BrowserWindow | null }): void {
    getMainWindow = deps.getMainWindow;

    // The renderer pushes its registry whenever a pane is registered, opened or
    // closed, so the tray can list panes it otherwise knows nothing about.
    // Fire-and-forget: the tray is a view of the renderer's truth.
    ipcMain.on(IPC_PANE_SYNC, (_event, panes: unknown) => {
        lastSync = Array.isArray(panes)
            ? panes.filter((p): p is TrayPane => !!p && typeof p.id === 'string' && typeof p.title === 'string')
            : [];
        refreshTray();
    });
}

export function getMainWindowRef(): BrowserWindow | null {
    return getMainWindow();
}
