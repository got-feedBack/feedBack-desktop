import { Menu, MenuItemConstructorOptions } from 'electron';

// Application menu.
//
// We previously relied on Electron's DEFAULT application menu. Its View
// submenu binds Zoom In to `CommandOrControl+Plus` only. On US / most
// keyboard layouts "+" is the SHIFTED form of the `=` key, so pressing
// Ctrl with the (unshifted) +/= key sends `Ctrl+=`, which the default
// accelerator does NOT match — Zoom In appears broken while Zoom Out
// (`Ctrl+-`, no Shift needed) works. That asymmetry is the reported bug.
//
// This template reproduces Electron's default menu via role-based submenus
// (so File/Edit/Window behaviour and their accelerators are unchanged) and
// hand-builds only the View submenu, where Zoom In additionally accepts the
// unshifted `=` key and the numpad `+`. Result: every way a user presses
// "Ctrl and plus" — `Ctrl+=`, `Ctrl+Shift+=` (the literal `+`), and numpad
// `Ctrl++` — now zooms in, restoring parity with Zoom Out.
export function installAppMenu(): void {
    const isMac = process.platform === 'darwin';

    const viewSubmenu: MenuItemConstructorOptions[] = [
        { role: 'reload' },
        { role: 'forceReload' },
        { role: 'toggleDevTools' },
        { type: 'separator' },
        { role: 'resetZoom' },
        // Primary, shown in the menu: the unshifted +/= key. This is the
        // keystroke the bug report says did nothing.
        { role: 'zoomIn', accelerator: 'CommandOrControl+=' },
        // Hidden siblings keep the other "plus" keystrokes working so this is
        // strictly additive (no regression for users who pressed Shift, and
        // numpad support for good measure). A MenuItem carries one accelerator,
        // so extra bindings are extra (invisible) items.
        { role: 'zoomIn', accelerator: 'CommandOrControl+Plus', visible: false },
        { role: 'zoomIn', accelerator: 'CommandOrControl+numadd', visible: false },
        { role: 'zoomOut' },
        { role: 'zoomOut', accelerator: 'CommandOrControl+numsub', visible: false },
        { type: 'separator' },
        { role: 'togglefullscreen' },
    ];

    const template: MenuItemConstructorOptions[] = [
        ...(isMac ? [{ role: 'appMenu' } as MenuItemConstructorOptions] : []),
        { role: 'fileMenu' },
        { role: 'editMenu' },
        { label: 'View', submenu: viewSubmenu },
        { role: 'windowMenu' },
    ];

    Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}
