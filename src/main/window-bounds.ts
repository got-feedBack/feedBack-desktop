// Pure window-bounds sanitization for main-window persistence. No electron
// imports so tests/window-bounds.test.js can load it headless via _load-ts.

export interface SavedWindowBounds {
    x: number;
    y: number;
    width: number;
    height: number;
    maximized?: boolean;
}

// A display's workArea (same shape as Electron's Rectangle).
export interface DisplayRect {
    x: number;
    y: number;
    width: number;
    height: number;
}

export interface RestoredWindowBounds {
    width: number;
    height: number;
    x?: number;
    y?: number;
    maximized: boolean;
}

// Keep in sync with createWindow() in main.ts.
export const DEFAULT_WIDTH = 1400;
export const DEFAULT_HEIGHT = 900;
export const MIN_WIDTH = 800;
export const MIN_HEIGHT = 600;

// Minimum overlap with some display's workArea for the saved position to be
// trusted — enough of the title bar to grab with the mouse.
const MIN_VISIBLE_W = 100;
const MIN_VISIBLE_H = 50;

function isFiniteNumber(v: unknown): v is number {
    return typeof v === 'number' && Number.isFinite(v);
}

// Size floor + fallback size. The main window's values are the defaults, so
// every existing call site behaves exactly as before; a pane window passes its
// own, because a 380x560 pane clamped to the main window's 800x600 floor would
// be silently inflated into something three times its intended size.
export interface WindowSizing {
    minWidth: number;
    minHeight: number;
    defaultWidth: number;
    defaultHeight: number;
}

const MAIN_WINDOW_SIZING: WindowSizing = {
    minWidth: MIN_WIDTH,
    minHeight: MIN_HEIGHT,
    defaultWidth: DEFAULT_WIDTH,
    defaultHeight: DEFAULT_HEIGHT,
};

// Validate saved bounds against the current display layout. Untrusted input
// (hand-edited/corrupt config, unplugged monitor, resolution change) degrades
// to defaults rather than producing an off-screen or absurd window. Omitted
// x/y means "let Electron center the window".
export function sanitizeWindowBounds(
    saved: unknown,
    displays: DisplayRect[],
    sizing: WindowSizing = MAIN_WINDOW_SIZING,
): RestoredWindowBounds {
    const defaults: RestoredWindowBounds = { width: sizing.defaultWidth, height: sizing.defaultHeight, maximized: false };
    if (displays.length === 0) return defaults;

    const b = saved as SavedWindowBounds | undefined;
    if (!b || typeof b !== 'object') return defaults;
    if (!isFiniteNumber(b.x) || !isFiniteNumber(b.y) || !isFiniteNumber(b.width) || !isFiniteNumber(b.height)) {
        return defaults;
    }
    const maximized = b.maximized === true;

    // Clamp size: never below the window minimums, never above the largest
    // display's workArea (window bigger than any screen → shrink to fit).
    const maxW = Math.max(...displays.map((d) => d.width));
    const maxH = Math.max(...displays.map((d) => d.height));
    const width = Math.min(Math.max(Math.round(b.width), sizing.minWidth), maxW);
    const height = Math.min(Math.max(Math.round(b.height), sizing.minHeight), maxH);

    // Trust the position only if the window meaningfully overlaps some
    // display. Negative coordinates are valid multi-monitor layouts — this is
    // an intersection test, not a sign check.
    const x = Math.round(b.x);
    const y = Math.round(b.y);
    const visible = displays.some((d) => {
        const overlapW = Math.min(x + width, d.x + d.width) - Math.max(x, d.x);
        const overlapH = Math.min(y + height, d.y + d.height) - Math.max(y, d.y);
        return overlapW >= MIN_VISIBLE_W && overlapH >= MIN_VISIBLE_H;
    });

    return visible ? { x, y, width, height, maximized } : { width, height, maximized };
}
