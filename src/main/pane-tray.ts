// The system tray.
//
// This is the first Tray in the app, and it exists for one reason: a pane you
// popped out is furniture. You want it out of the way while you play and back
// instantly when you don't — without hunting for it behind the main window or in
// a taskbar full of small companions.
//
// The menu is a view of the RENDERER's pane registry (pushed up over pane:sync),
// not a second copy of it. Main never decides what a pane is; it only shows and
// hides windows. A pane the tray doesn't have a window for is toggled by asking
// the renderer to open it — which is what pane:toggle is.
//
// Icon: resolved from dist/main/, next to the compiled JS. build:ts copies it
// there, which is the same trick splash.html/spinner.json use — it means
// `__dirname` works identically in dev and in a packaged asar, with no
// app.isPackaged branch and nothing to add to electron-builder's extraResources.

import { Menu, Tray, nativeImage, app } from 'electron';
import * as path from 'path';
import { IPC_PANE_EVENT_TOGGLE } from './ipc-channels';

export interface TrayPane {
    id: string;
    title: string;
    icon?: string;
    open?: boolean;
}

// What the tray needs from the pane host, INJECTED rather than imported.
//
// pane-hosts imports setTrayPanes from this file, so importing back from there
// would make the two modules mutually dependent — and a require cycle in the main
// process is not a style question: whichever module loads second sees the other's
// exports half-initialised, which fails at whatever moment the graph happens to
// resolve in. One direction only: pane-hosts → pane-tray, and the actions come in
// through initTray().
export interface TrayPaneActions {
    getMainWindow: () => Electron.BrowserWindow | null;
    // Returns true if it owned a window for this pane and toggled it; false if it
    // did not, in which case only the renderer can decide what opening it means.
    toggleWindow: (paneId: string) => boolean;
    showAll: () => void;
    hideAll: () => void;
}

let tray: Tray | null = null;
let panes: TrayPane[] = [];
let actions: TrayPaneActions | null = null;

function iconPath(): string {
    // Windows wants an .ico; macOS and Linux take a PNG. build:ts copies
    // resources/icons/icon.ico -> tray.ico and resources/icons/32x32.png -> tray.png.
    //
    // macOS additionally wants a monochrome TEMPLATE image, which a 32px colour PNG
    // is not — so the tray icon will render in colour there rather than adapting to
    // light/dark menu bars. Acceptable, and far preferable to shipping no tray at
    // all; a proper …Template.png is a follow-up.
    return path.join(__dirname, process.platform === 'win32' ? 'tray.ico' : 'tray.png');
}

function showMainWindow(): void {
    const win = actions?.getMainWindow() ?? null;
    if (!win || win.isDestroyed()) return;
    if (win.isMinimized()) win.restore();
    win.show();
    win.focus();
}

function buildMenu(): Menu {
    const paneItems: Electron.MenuItemConstructorOptions[] = panes.map((p) => ({
        label: (p.icon ? p.icon + '  ' : '') + p.title,
        type: 'checkbox',
        checked: p.open === true,
        click: () => {
            // Let toggleWindow() be the authority, rather than asking "do we have a
            // window?" and then acting on the answer. Windows are destroyed
            // asynchronously, so between the question and the act the answer can go
            // stale — and the click would land on nothing and silently do nothing.
            //
            // If it toggled, we're done. If it didn't, we never had that window (or
            // just lost it), and only the renderer can decide what opening the pane
            // means — it might belong in the dock, and its element lives there.
            if (actions?.toggleWindow(p.id)) return;
            const win = actions?.getMainWindow() ?? null;
            if (win && !win.isDestroyed()) win.webContents.send(IPC_PANE_EVENT_TOGGLE, { paneId: p.id });
        },
    }));

    const template: Electron.MenuItemConstructorOptions[] = [
        { label: 'Show fee[dB]ack', click: showMainWindow },
        { type: 'separator' },
    ];

    if (paneItems.length) {
        template.push({ label: 'Panes', enabled: false });
        template.push(...paneItems);
        template.push({ type: 'separator' });
        template.push({ label: 'Show all panes', click: () => actions?.showAll() });
        template.push({ label: 'Hide all panes', click: () => actions?.hideAll() });
    } else {
        template.push({ label: 'No panes', enabled: false });
    }

    template.push({ type: 'separator' });
    template.push({ label: 'Quit fee[dB]ack', click: () => app.quit() });

    return Menu.buildFromTemplate(template);
}

// Called by pane-hosts whenever the registry or the window state changes. The
// whole menu is rebuilt — it is a handful of items, built only on user-visible
// state changes, and never on a playback path.
export function setTrayPanes(next: TrayPane[]): void {
    panes = next;
    if (!tray || tray.isDestroyed()) return;
    tray.setContextMenu(buildMenu());
}

export function initTray(deps: TrayPaneActions): void {
    if (tray) return;
    actions = deps;

    const image = nativeImage.createFromPath(iconPath());
    if (image.isEmpty()) {
        // A Tray built from an empty image is an invisible tray: the menu exists
        // but the user can never reach it. Fail loudly and simply go without —
        // panes still pop out, they just aren't tray-managed.
        console.warn(`[panes] tray icon missing or unreadable at ${iconPath()} — running without a tray`);
        return;
    }

    tray = new Tray(image);
    tray.setToolTip('fee[dB]ack');
    tray.setContextMenu(buildMenu());
    // Left-click is the fast path back to the app, which is what a user reaching
    // for the tray almost always wants. (No-op on Linux, where most desktops give
    // a left-click the context menu anyway.)
    tray.on('click', showMainWindow);
}

export function destroyTray(): void {
    if (tray && !tray.isDestroyed()) tray.destroy();
    tray = null;
}
