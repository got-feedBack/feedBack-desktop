const test = require('node:test');
const assert = require('node:assert/strict');
const { loadTs } = require('./_load-ts');

const { sanitizeWindowBounds, DEFAULT_WIDTH, DEFAULT_HEIGHT, MIN_WIDTH, MIN_HEIGHT } =
    loadTs('src/main/window-bounds.ts');

// A common single-display workArea (1920×1080 minus a 40px taskbar).
const PRIMARY = { x: 0, y: 0, width: 1920, height: 1040 };
const DEFAULTS = { width: DEFAULT_WIDTH, height: DEFAULT_HEIGHT, maximized: false };

test('valid bounds on a matching display round-trip', () => {
    const saved = { x: 100, y: 50, width: 1200, height: 800, maximized: true };
    assert.deepEqual(sanitizeWindowBounds(saved, [PRIMARY]), saved);
});

test('garbage input falls back to defaults', () => {
    for (const bad of [undefined, null, 'wat', 42, {}, { x: 1, y: 2 }, { x: 'a', y: 0, width: 1200, height: 800 }, { x: NaN, y: 0, width: 1200, height: 800 }, { x: 0, y: 0, width: Infinity, height: 800 }]) {
        assert.deepEqual(sanitizeWindowBounds(bad, [PRIMARY]), DEFAULTS);
    }
});

test('empty display list falls back to defaults', () => {
    assert.deepEqual(sanitizeWindowBounds({ x: 0, y: 0, width: 1200, height: 800 }, []), DEFAULTS);
});

test('oversize bounds clamp to the largest display workArea', () => {
    const out = sanitizeWindowBounds({ x: 0, y: 0, width: 5000, height: 4000 }, [PRIMARY]);
    assert.deepEqual(out, { x: 0, y: 0, width: 1920, height: 1040, maximized: false });
});

test('undersize bounds clamp up to the window minimums', () => {
    const out = sanitizeWindowBounds({ x: 10, y: 10, width: 300, height: 200 }, [PRIMARY]);
    assert.deepEqual(out, { x: 10, y: 10, width: MIN_WIDTH, height: MIN_HEIGHT, maximized: false });
});

test('position on a now-unplugged monitor is dropped, size kept', () => {
    // Saved on a second display to the right that no longer exists.
    const out = sanitizeWindowBounds({ x: 2000, y: 100, width: 1200, height: 800 }, [PRIMARY]);
    assert.deepEqual(out, { width: 1200, height: 800, maximized: false });
});

test('negative coordinates on a left-of-primary monitor are kept', () => {
    const leftMonitor = { x: -1920, y: 0, width: 1920, height: 1040 };
    const saved = { x: -1800, y: 100, width: 1200, height: 800 };
    const out = sanitizeWindowBounds(saved, [leftMonitor, PRIMARY]);
    assert.deepEqual(out, { ...saved, maximized: false });
});

test('sliver overlap below the grab threshold drops the position', () => {
    // Only 50px of the window's left edge on screen — not enough to grab.
    const out = sanitizeWindowBounds({ x: 1870, y: 100, width: 1200, height: 800 }, [PRIMARY]);
    assert.deepEqual(out, { width: 1200, height: 800, maximized: false });
});

test('fractional coordinates are rounded to integers', () => {
    const out = sanitizeWindowBounds({ x: 10.6, y: 20.4, width: 1200.5, height: 800.2 }, [PRIMARY]);
    assert.deepEqual(out, { x: 11, y: 20, width: 1201, height: 800, maximized: false });
});
