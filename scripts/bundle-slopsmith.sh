#!/bin/bash
# Bundle the Slopsmith server source + plugins into resources/slopsmith/.
#
# Slopsmith repo location is resolved in this order:
#   1. $SLOPSMITH_DIR env var
#   2. ../slopsmith (sibling to this repo)
#   3. ~/Repositories/slopsmith (legacy dev layout)
#
# Cross-platform: avoids `readlink -f` (not available on macOS by default)
# by using python's os.path.realpath. `rsync` is used for the resolved-
# symlink copy step and must be present (see .packages/apt.txt).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUNDLE_DIR="$PROJECT_DIR/resources/slopsmith"

if [ -z "${SLOPSMITH_DIR:-}" ]; then
    if [ -d "$PROJECT_DIR/../slopsmith" ]; then
        SLOPSMITH_DIR="$PROJECT_DIR/../slopsmith"
    elif [ -d "$HOME/Repositories/slopsmith" ]; then
        SLOPSMITH_DIR="$HOME/Repositories/slopsmith"
    else
        SLOPSMITH_DIR=""
    fi
fi

if [ -z "$SLOPSMITH_DIR" ] || [ ! -d "$SLOPSMITH_DIR" ]; then
    echo "ERROR: Slopsmith repository not found." >&2
    echo "Searched:" >&2
    echo "  \$SLOPSMITH_DIR (unset)" >&2
    echo "  $PROJECT_DIR/../slopsmith" >&2
    echo "  $HOME/Repositories/slopsmith" >&2
    echo "Clone it with: git clone https://github.com/got-feedback/feedback.git ../slopsmith" >&2
    exit 1
fi

# Portable realpath — readlink -f doesn't exist on stock macOS.
realpath_portable() {
    python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1"
}

echo "=== Bundling Slopsmith server and plugins ==="
echo "  Source: $SLOPSMITH_DIR"

rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/static" "$BUNDLE_DIR/plugins"

# Server + lib. Copy ALL top-level .py modules, not just server.py — core
# grows sibling modules (e.g. appstate.py, added in core#833) and a stale
# whitelist here ships a server.py whose imports are missing, crashing the
# packaged backend with ModuleNotFoundError on startup.
cp "$SLOPSMITH_DIR"/*.py "$BUNDLE_DIR/"
cp "$SLOPSMITH_DIR/VERSION" "$BUNDLE_DIR/"
# Copy every top-level python package (dir with __init__.py), not a hardcoded
# list — core's R3 refactor keeps adding packages (routers/ in core#834, lib
# extractions in #830/#831) and each miss ships a backend that dies on import.
# plugins/ is bundled selectively further down; tests/ must never ship.
for pkg in "$SLOPSMITH_DIR"/*/__init__.py; do
    [ -f "$pkg" ] || continue
    pkg_dir="$(dirname "$pkg")"
    case "$(basename "$pkg_dir")" in
        plugins|tests) continue ;;
    esac
    cp -r "$pkg_dir" "$BUNDLE_DIR/"
done
cp -r "$SLOPSMITH_DIR/lib" "$BUNDLE_DIR/"
find "$BUNDLE_DIR" -type d -name '__pycache__' -prune -exec rm -rf {} +

# Bundled content (progression paths, quests, shop definitions)
[ -d "$SLOPSMITH_DIR/data" ] && cp -r "$SLOPSMITH_DIR/data" "$BUNDLE_DIR/"

# Static assets — copy the whole directory. User-data dirs (art/, sloppak_cache/)
# and generated audio_*.mp3 files are gitignored and won't exist in a clean checkout.
cp -r "$SLOPSMITH_DIR/static/." "$BUNDLE_DIR/static/"
# Strip any leftover user-data that may exist in a dev checkout.
rm -rf "$BUNDLE_DIR/static/art" "$BUNDLE_DIR/static/sloppak_cache"
find "$BUNDLE_DIR/static" -maxdepth 1 -name 'audio_*.mp3' -delete

# Builtin diagnostic sloppak — server._seed_builtin_diagnostic_sloppaks() copies
# this into DLC_DIR/diagnostics-builtin/ on library scan startup. The source path
# must match server.py's _BUILTIN_DIAGNOSTIC_SOURCES (feedBack-*, not slopsmith-*).
DIAG_SLOPPAK="$SLOPSMITH_DIR/docs/diagnostics/feedBack-diagnostic-basic-guitar.sloppak"
if [ -f "$DIAG_SLOPPAK" ]; then
    mkdir -p "$BUNDLE_DIR/docs/diagnostics"
    cp "$DIAG_SLOPPAK" "$BUNDLE_DIR/docs/diagnostics/"
else
    echo "WARNING: diagnostic sloppak not found at $DIAG_SLOPPAK — builtin seeding will skip in packaged builds" >&2
fi

# Builtin starter content — server._seed_builtin_starter_content() copies these
# into DLC_DIR/starter/ once on first run so a fresh install ships a playable
# library. Source path must match server.py's _BUILTIN_STARTER_SOURCES.
STARTER_SRC="$SLOPSMITH_DIR/content/starter"
if [ -d "$STARTER_SRC" ]; then
    # Explicit no-match detection ([ -e ]) instead of relying on cp failing —
    # so a real cp error (unreadable source, disk full) still aborts under
    # `set -e` rather than being swallowed as "no starter feedpaks".
    starter_paks=("$STARTER_SRC"/*.feedpak)
    if [ -e "${starter_paks[0]:-}" ]; then
        mkdir -p "$BUNDLE_DIR/content/starter"
        cp "${starter_paks[@]}" "$BUNDLE_DIR/content/starter/"
        echo "  Bundled starter content: ${#starter_paks[@]} feedpak(s)"
    else
        echo "WARNING: no starter feedpaks in $STARTER_SRC — starter seeding will skip in packaged builds" >&2
    fi
else
    echo "WARNING: content/starter not found — starter seeding will skip in packaged builds" >&2
fi

# Cross-platform "cp -r minus .git" — Git Bash on Windows doesn't ship
# rsync, so we can't rely on `rsync --exclude=.git`. Plain `cp -r`
# followed by stripping any nested `.git/` directories works on
# Linux/macOS/Git-Bash alike. The .git stripping matters because
# plugin directories cloned by clone_slopsmith() are git working trees;
# bundling their .git/objects/ would inflate the .app and (on macOS)
# trip electron-builder with EACCES on read-only pack files.
copy_plugin() {
    local src="$1"
    local dst="$2"
    mkdir -p "$dst"
    # Use cp -R rather than -r for portable symlink-following semantics.
    cp -R "$src/." "$dst/"
    find "$dst" -name '.git' -type d -prune -exec rm -rf {} +
}

# Built-in plugins (real directories, not symlinks to avoid duplicates).
for plugin_dir in "$SLOPSMITH_DIR/plugins/editor" "$SLOPSMITH_DIR/plugins/note_detect"; do
    if [ -d "$plugin_dir" ] && [ ! -L "$plugin_dir" ]; then
        name=$(basename "$plugin_dir")
        copy_plugin "$plugin_dir" "$BUNDLE_DIR/plugins/$name"
    fi
done

# External plugins: resolve symlinks, skip .git
for plugin_link in "$SLOPSMITH_DIR/plugins/"*; do
    name=$(basename "$plugin_link")
    [ "$name" = "__pycache__" ] && continue
    [ "$name" = "__init__.py" ] && continue
    target="$BUNDLE_DIR/plugins/$name"
    [ -d "$target" ] && continue  # already copied

    if [ -L "$plugin_link" ]; then
        real_dir=$(realpath_portable "$plugin_link")
        if [ -d "$real_dir" ]; then
            copy_plugin "$real_dir" "$target"
        fi
    elif [ -d "$plugin_link" ]; then
        copy_plugin "$plugin_link" "$target"
    elif [ -f "$plugin_link" ]; then
        cp "$plugin_link" "$target"
    fi
done

# Plugin-discovery __init__.py
cp "$SLOPSMITH_DIR/plugins/__init__.py" "$BUNDLE_DIR/plugins/"

# Desktop-specific plugins (audio_engine, plugin_manager) declared in
# src/renderer/**/plugin.json
for dp in "$PROJECT_DIR/src/renderer" "$PROJECT_DIR/src/renderer/plugin-manager"; do
    if [ -f "$dp/plugin.json" ]; then
        pname=$(python3 -c "import json, sys; print(json.load(open(sys.argv[1]))['id'])" "$dp/plugin.json")
        mkdir -p "$BUNDLE_DIR/plugins/$pname"
        cp "$dp"/*.html "$dp"/*.js "$dp"/plugin.json "$BUNDLE_DIR/plugins/$pname/" 2>/dev/null || true
    fi
done

# ── Rebuild Tailwind CSS over the FULL bundled plugin set ──────────────────
# Core's committed static/tailwind.min.css is built scanning only the in-tree
# plugins (highway_3d, editor, note_detect, app_tour_*). Shipped as-is it would
# leave most of the 30+ bundled plugins' classes unstyled — the Play CDN's
# runtime JIT used to cover them, but it was removed (slopsmith#411). So
# regenerate the sheet HERE, after every plugin is copied, so it covers the
# whole bundled set and scales automatically as more plugins are added.
#
# We reuse core's tailwind.config.js for parity (same theme colors, safelist,
# and the highway_3d exclusion — that plugin ships its own assets/plugin.css
# via the `styles` capability). The config is copied into the bundle and run
# from there so its relative content globs (./static/**, ./plugins/**) resolve
# against the bundle regardless of Tailwind's cwd-vs-config-dir semantics.
if command -v npx >/dev/null 2>&1; then
    echo "=== Rebuilding Tailwind CSS over bundled plugins ==="
    # Whole pipeline runs inside one guarded `if (...)` so ANY failure (config
    # copy, no npm cache/network, build error, or the final swap) falls back to
    # the committed sheet instead of aborting the bundle under `set -e`. Two
    # subtleties: (1) `set -e` is suppressed inside an `if` condition, so the
    # steps are &&-chained to make an early failure short-circuit; (2) the build
    # writes to a temp file that's mv'd into place only as the last link, so a
    # failed/partial `npx` can never truncate the committed fallback sheet.
    if (
        cp "$SLOPSMITH_DIR/tailwind.config.js" "$BUNDLE_DIR/tailwind.config.js" \
        && cd "$BUNDLE_DIR" \
        && npx -y tailwindcss@3.4.19 \
            -c tailwind.config.js \
            -i static/_tailwind.src.css \
            -o static/tailwind.min.css.new \
            --minify \
        && mv -f static/tailwind.min.css.new static/tailwind.min.css
    ); then
        # Drop the build-only inputs so they don't ship in resources/slopsmith.
        # `|| true`: cleanup is best-effort — a stray rm failure (e.g. Windows
        # file locks) must never abort the bundle under `set -e`.
        rm -f "$BUNDLE_DIR/tailwind.config.js" "$BUNDLE_DIR/static/_tailwind.src.css" || true
        echo "  Tailwind CSS: $(wc -c < "$BUNDLE_DIR/static/tailwind.min.css") bytes (bundled-plugin-aware)"
    else
        # Discard any partial output; the committed sheet copied earlier stays
        # intact. Still drop the build-only input so it never ships (matches the
        # success path). `|| true` keeps cleanup non-fatal.
        rm -f "$BUNDLE_DIR/tailwind.config.js" "$BUNDLE_DIR/static/tailwind.min.css.new" "$BUNDLE_DIR/static/_tailwind.src.css" || true
        echo "WARN: Tailwind rebuild failed — shipping core's committed sheet as-is." >&2
        echo "      Bundled plugins using classes outside it may render unstyled." >&2
    fi
else
    # No rebuild engine; still drop the build-only input so bundle contents are
    # consistent regardless of whether the rebuild ran. `|| true` keeps it
    # non-fatal under `set -e`.
    rm -f "$BUNDLE_DIR/static/_tailwind.src.css" || true
    echo "WARN: npx/node not found — shipping core's committed tailwind.min.css as-is." >&2
    echo "      Bundled plugins using classes outside core's sheet may render unstyled." >&2
fi

echo "  Slopsmith server: $(du -sh "$BUNDLE_DIR" | cut -f1)"
echo "  Plugins: $(ls -d "$BUNDLE_DIR/plugins/"*/ 2>/dev/null | wc -l)"
echo "=== Slopsmith bundle complete ==="
