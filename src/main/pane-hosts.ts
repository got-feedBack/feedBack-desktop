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

// The config file is untrusted: hand-edited, corrupt, or written by a build that
// disagrees with this one. It is read in the MAIN process, where a TypeError is not
// a bad pane — it is the app failing to start.
function savedPaneMap(): Record<string, unknown> {
    const map = getDesktopConfig().paneWindows;
    if (!map || typeof map !== 'object' || Array.isArray(map)) return {};
    return map as Record<string, unknown>;
}

function savedFor(paneId: string): SavedPaneWindow {
    const saved = savedPaneMap();
    // Own-property check: a polluted or hand-edited config would otherwise hand back
    // a value off the prototype chain for a pane that was never saved at all.
    if (!Object.prototype.hasOwnProperty.call(saved, paneId)) return {};
    const entry = saved[paneId];
    // `{"camera_director": null}` passes the own-property check and then explodes on
    // `saved.bounds`. Anything that is not an object degrades to "nothing saved",
    // which is exactly what an unreadable entry means.
    if (!entry || typeof entry !== 'object' || Array.isArray(entry)) return {};
    return entry as SavedPaneWindow;
}

// A pane id reaches us from the RENDERER (it is the tail of the frame name a
// window.open() supplied), and it is used as a key in the persisted map. So it is
// untrusted input in key position: `__proto__` and friends are not ids, they are a
// way to mutate Object.prototype from a plugin.
const UNSAFE_KEYS = new Set(['__proto__', 'constructor', 'prototype']);
function isUnsafePaneId(paneId: string): boolean {
    return UNSAFE_KEYS.has(paneId);
}

function persist(paneId: string, patch: SavedPaneWindow): void {
    if (isUnsafePaneId(paneId)) return;
    try {
        // setDesktopConfig merges shallowly, so paneWindows must be
        // read-modify-written or one pane's save would drop every other pane's.
        //
        // Built on a null-prototype object: whatever is in the config file (hand
        // edited, corrupt, or written by an older build) cannot smuggle a prototype
        // into a map we then write keys onto.
        const all: Record<string, SavedPaneWindow> = Object.create(null);
        const saved = savedPaneMap();
        for (const key of Object.keys(saved)) {
            if (isUnsafePaneId(key)) continue;
            const entry = saved[key];
            // Don't carry a corrupt entry forward. Spreading `null` into the patch
            // below would be silently fine; writing it back out would keep a value
            // that crashes savedFor() on the next launch forever.
            if (!entry || typeof entry !== 'object' || Array.isArray(entry)) continue;
            all[key] = entry as SavedPaneWindow;
        }
        all[paneId] = { ...(all[paneId] ?? {}), ...patch };
        setDesktopConfig({ paneWindows: { ...all } });
    } catch (err) {
        console.warn(`[panes] failed to persist geometry for ${paneId}:`, err);
    }
}

// Everything we remember about a pane window, read in one place so the debounced
// save, the flush-on-close and the flush-on-quit can never disagree about what
// "remembered" means. alwaysOnTop is in here because it was being RESTORED and never
// written — a setting that could only ever be turned on by hand-editing the config.
function snapshot(win: BrowserWindow): SavedPaneWindow {
    return {
        bounds: { ...win.getNormalBounds(), maximized: false },
        alwaysOnTop: win.isAlwaysOnTop(),
    };
}

// setDesktopConfig is writeFileSync + renameSync. Wiring that straight to 'moved'
// and 'resized' means a synchronous disk write for every frame of a drag — on
// macOS, dozens per second, in the main process, where they block everything else.
// Write once the gesture settles instead.
const GEOMETRY_SAVE_DEBOUNCE_MS = 400;
const saveTimers = new Map<string, NodeJS.Timeout>();

function persistSoon(paneId: string, patch: () => SavedPaneWindow | null): void {
    clearTimeout(saveTimers.get(paneId));
    saveTimers.set(paneId, setTimeout(() => {
        saveTimers.delete(paneId);
        const value = patch();
        if (value) persist(paneId, value);
    }, GEOMETRY_SAVE_DEBOUNCE_MS));
}

// Debouncing means the last move can still be in flight when the window goes. Write
// it out NOW, while the window is alive enough to be measured — otherwise a user who
// nudges a pane and closes it a moment later loses the position they just chose,
// which is exactly the thing remembered geometry exists to prevent.
function flushGeometry(win: BrowserWindow, paneId: string): void {
    const timer = saveTimers.get(paneId);
    if (timer) { clearTimeout(timer); saveTimers.delete(paneId); }
    if (win.isDestroyed()) return;
    persist(paneId, snapshot(win));
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

    // A PANE CAN NEVER GO BEHIND THE APP.
    //
    // A pane is a control surface for the thing you are looking at. Clicking the
    // highway to play — the single most common thing anyone does here — raises the
    // main window, and a plain sibling window would slide straight behind it. You
    // would have to fish the mixer back out from behind the game every time you
    // touched the game. That is not a pop-out, it is a hiding place.
    //
    // Parenting to the main window (rather than setAlwaysOnTop) is the precise
    // amount of "in front": the pane always floats above fee[dB]ack, and behaves
    // like any other window against everything else. Always-on-top would put it
    // above the user's browser and editor too — a surprising thing to inflict on
    // someone for opening a mixer.
    //
    // alwaysOnTop remains available on top of this for a pane the user explicitly
    // wants above EVERYTHING; the two compose.
    const main = getMainWindow();
    if (main && !main.isDestroyed()) {
        try {
            win.setParentWindow(main);
        } catch (err) {
            // Not fatal: a pane that can be buried is still a working pane.
            console.warn(`[panes] could not parent ${paneId} to the main window:`, err);
        }
    }

    // A pane is a companion to the app, not an entry to it: keep it off the taskbar
    // so it never masquerades as a second fee[dB]ack.
    win.setSkipTaskbar(true);

    // Persist on move/resize, not only on close — a pane window can outlive the app
    // in a crash, and the whole point of remembering geometry is that you never
    // place it twice.
    const save = (): void => {
        persistSoon(paneId, () => (win.isDestroyed() ? null : snapshot(win)));
    };
    win.on('moved', save);
    win.on('resized', save);

    // NO MINIMIZE BUTTON. This is a safety rail, not a style choice.
    //
    // A pane window is skipTaskbar (it must not masquerade as a second fee[dB]ack)
    // and, since it is parented to the main window, an OWNED window — which Windows
    // will not give a taskbar button to in any case. So a minimized pane has no
    // taskbar entry and no alt-tab entry. The only route back is the tray, and
    // Windows hides new tray icons behind the overflow chevron by default.
    //
    // Net effect if we allow it: the user clicks minimize and the window is gone.
    // Not hidden — gone, with no affordance anywhere on screen to bring it back.
    // That is exactly what happened in testing, and no amount of "it's in the tray"
    // makes it acceptable.
    //
    // So the button is removed. Every way of putting a pane away that remains is one
    // the user can undo from something they can see:
    //   - close it        → the panel returns to the app, where it came from
    //   - the tray        → per-pane toggle, and Show/Hide all panes
    //   - the chip's stub → in the app, dead centre of where the panel used to be
    win.setMinimizable(false);

    // Belt and braces: if something else minimizes it anyway (a window manager, a
    // shortcut, a future code path), restore it rather than leave it unreachable.
    win.on('minimize', () => {
        win.restore();
    });

    // 'close' fires while the window still exists; 'closed' after it is gone. The
    // final geometry can only be read from the former.
    win.on('close', () => flushGeometry(win, paneId));

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
    // destroy() does NOT fire 'close', so the flush wired to that event never runs on
    // this path — and the debounced save may still be pending. Move a pane, quit two
    // seconds later, and its position would be gone. Flush every pane first.
    windows.forEach((win, paneId) => flushGeometry(win, paneId));

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

// A pane is sent to the tray by MINIMIZING it and then hiding it — and hiding a
// minimized window does not un-minimize it. So show() alone would restore a window
// that is still minimized: present, but not on screen, which reads as the tray
// being broken. Always restore first.
function reveal(win: BrowserWindow): void {
    if (win.isDestroyed()) return;
    if (win.isMinimized()) win.restore();
    win.show();
}

export function togglePaneWindow(paneId: string): boolean {
    const win = windows.get(paneId);
    if (!win || win.isDestroyed()) return false;   // not open → only the renderer can open it
    if (win.isVisible() && !win.isMinimized()) win.hide(); else reveal(win);
    refreshTray();
    return true;
}

export function showAllPaneWindows(): void {
    windows.forEach((win) => { if (!win.isDestroyed()) reveal(win); });
    refreshTray();
}

export function hideAllPaneWindows(): void {
    windows.forEach((win) => { if (!win.isDestroyed() && win.isVisible()) win.hide(); });
    refreshTray();
}

// ── Wiring ──────────────────────────────────────────────────────────────────

export function initPaneHosts(deps: { getMainWindow: () => BrowserWindow | null }): void {
    getMainWindow = deps.getMainWindow;

    // The renderer pushes its registry whenever a pane is registered, opened or
    // closed, so the tray can list panes it otherwise knows nothing about.
    // Fire-and-forget: the tray is a view of the renderer's truth.
    //
    // Only the MAIN window's truth, though. Pane windows are same-origin top-level
    // frames, so preload.ts's isMainFrame gate gives them the bridge too — meaning a
    // pane window (or any allowed pop-up) could send pane:sync and overwrite the
    // tray's registry, most simply by pushing an empty list and emptying the menu.
    // Only one renderer owns the pane registry; accept it from that one only.
    ipcMain.on(IPC_PANE_SYNC, (event, panes: unknown) => {
        const main = getMainWindow();
        if (!main || main.isDestroyed() || event.sender !== main.webContents) {
            console.warn('[panes] ignoring pane:sync from a webContents that is not the main window');
            return;
        }
        lastSync = Array.isArray(panes)
            ? panes.filter((p): p is TrayPane => !!p && typeof p.id === 'string' && typeof p.title === 'string')
            : [];
        refreshTray();
    });
}
