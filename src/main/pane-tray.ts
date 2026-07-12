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
import { togglePaneWindow, showAllPaneWindows, hideAllPaneWindows, hasPaneWindow } from './pane-hosts';

export interface TrayPane {
    id: string;
    title: string;
    icon?: string;
    open?: boolean;
}

let tray: Tray | null = null;
let panes: TrayPane[] = [];
let getMainWindow: () => Electron.BrowserWindow | null = () => null;

function iconPath(): string {
    // Windows wants an .ico; macOS and Linux take a PNG. macOS additionally wants
    // a monochrome template image, which the 16px PNG is not — so it will render
    // in colour there. Acceptable, and preferable to shipping no tray at all;
    // a proper …Template.png is a follow-up.
    return path.join(__dirname, process.platform === 'win32' ? 'tray.ico' : 'tray.png');
}

function showMainWindow(): void {
    const win = getMainWindow();
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
            // If we already own a window for this pane, showing/hiding it is a
            // main-process job and instant. If we don't, only the renderer can
            // decide what opening it means (it might belong in the dock), so ask.
            if (hasPaneWindow(p.id)) { togglePaneWindow(p.id); return; }
            const win = getMainWindow();
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
        template.push({ label: 'Show all panes', click: showAllPaneWindows });
        template.push({ label: 'Hide all panes', click: hideAllPaneWindows });
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

export function initTray(deps: { getMainWindow: () => Electron.BrowserWindow | null }): void {
    if (tray) return;
    getMainWindow = deps.getMainWindow;

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
