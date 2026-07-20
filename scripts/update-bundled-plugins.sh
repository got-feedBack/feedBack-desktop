#!/usr/bin/env bash
# Refresh the installed desktop plugins from the same manifest used by CI.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="$SCRIPT_DIR/bundled-plugins.txt"
source "$SCRIPT_DIR/plugin-manifest.sh"

case "$(uname -s)" in
	Darwin*) DEFAULT_PLUGIN_DIR="$HOME/Library/Application Support/feedback-desktop/plugins" ;;
	MINGW*|MSYS*|CYGWIN*)
		[[ -n "${APPDATA:-}" ]] || {
			echo "Error: APPDATA is not set; cannot locate the Windows plugin directory" >&2
			exit 1
		}
		windows_appdata="$APPDATA"
		if command -v cygpath >/dev/null; then
			windows_appdata="$(cygpath -u "$windows_appdata")"
		fi
		DEFAULT_PLUGIN_DIR="$windows_appdata/feedback-desktop/plugins"
		;;
	*) DEFAULT_PLUGIN_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/feedback-desktop/plugins" ;;
esac
PLUGIN_DIR="${FEEDBACK_PLUGIN_DIR:-$DEFAULT_PLUGIN_DIR}"
DRY_RUN=false

usage() {
	cat <<EOF
Usage: $(basename "$0") [--dry-run] [plugin-directory]

Clones every release plugin at its configured branch (or its default branch)
and synchronizes it into the desktop plugin directory. Plugins newly added to
$MANIFEST are created automatically.

Run this script from Bash (Git Bash on Windows). Set GH_CLONE_TOKEN for private
repositories, or FEEDBACK_PLUGIN_DIR to change the platform default:
  $DEFAULT_PLUGIN_DIR

Close feedBack Desktop before applying updates so Windows does not hold plugin
files open. --dry-run is safe while the application is running.
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
		--dry-run) DRY_RUN=true ;;
		-h|--help) usage; exit 0 ;;
		-*) echo "Error: unknown option: $1" >&2; usage >&2; exit 2 ;;
		*) PLUGIN_DIR="$1" ;;
	esac
	shift
done

command -v git >/dev/null || { echo "Error: git is required" >&2; exit 1; }
[[ -d "$PLUGIN_DIR" ]] || {
	echo "Error: plugin directory does not exist: $PLUGIN_DIR" >&2
	echo "Start feedBack Desktop once, or pass the plugin directory explicitly." >&2
	exit 1
}

tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/feedback-plugin-update.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT

auth=""
[[ -n "${GH_CLONE_TOKEN:-}" ]] && auth="x-access-token:${GH_CLONE_TOKEN}@"
total=0
updated=0
unchanged=0
failed=0

while IFS= read -r entry; do
	total=$((total + 1))
	parse_plugin_entry "$entry"
	source_dir="$tmp_dir/$PLUGIN_DIRNAME"
	destination="$PLUGIN_DIR/$PLUGIN_DIRNAME"
	clone_args=(--depth 1)
	[[ -n "$PLUGIN_BRANCH" ]] && clone_args+=(--branch "$PLUGIN_BRANCH")

	printf '[%d] %s%s -> %s\n' "$total" "$PLUGIN_OWNER_REPO" \
		"${PLUGIN_BRANCH:+@$PLUGIN_BRANCH}" "$PLUGIN_DIRNAME"
	if ! git clone -q "${clone_args[@]}" \
		"https://${auth}github.com/${PLUGIN_OWNER_REPO}.git" "$source_dir"; then
		echo "  ERROR: clone failed; installed copy was left unchanged" >&2
		failed=$((failed + 1))
		continue
	fi

	# Compare only files tracked by the fetched plugin commit. Runtime-created
	# files in the installed directory (for example __pycache__) are deliberately
	# ignored. --no-ext-diff and --no-textconv keep the check deterministic and
	# prevent a contributor's Git configuration from launching external tools.
	if [[ -d "$destination" ]] && git \
		--git-dir="$source_dir/.git" \
		--work-tree="$destination" \
		-c core.safecrlf=false \
		diff --quiet --no-ext-diff --no-textconv HEAD --; then
		echo "  already up to date"
		unchanged=$((unchanged + 1))
		continue
	fi

	# Release bundles do not contain repository metadata. Removing it here also
	# avoids shipping Windows-incompatible Git worktree files into AppData.
	rm -rf "$source_dir/.git"

	if [[ "$DRY_RUN" == true ]]; then
		if [[ -d "$destination" ]]; then
			echo "  would replace existing plugin"
		else
			echo "  would install new plugin"
		fi
	else
		# A directory swap works in macOS/Linux Bash and stock Git Bash, where
		# rsync is not installed. Keep the old directory until the new one is
		# successfully in place so an interrupted move can be rolled back.
		backup="$PLUGIN_DIR/.feedback-plugin-backup-$PLUGIN_DIRNAME-$$"
		if [[ -e "$backup" ]]; then
			echo "  ERROR: stale backup exists: $backup" >&2
			failed=$((failed + 1))
			continue
		fi
		if [[ -e "$destination" ]] && ! mv "$destination" "$backup"; then
			echo "  ERROR: could not move installed copy (is the app running?)" >&2
			failed=$((failed + 1))
			continue
		fi
		if mv "$source_dir" "$destination"; then
			rm -rf "$backup"
		else
			echo "  ERROR: install failed; restoring previous copy" >&2
			rm -rf "$destination"
			[[ ! -e "$backup" ]] || mv "$backup" "$destination"
			failed=$((failed + 1))
			continue
		fi
	fi
	updated=$((updated + 1))
done < <(plugin_manifest_entries "$MANIFEST")

if [[ "$DRY_RUN" == true ]]; then
	echo "Would update $updated of $total release plugins ($unchanged already up to date) in: $PLUGIN_DIR"
else
	echo "Updated $updated of $total release plugins ($unchanged already up to date) in: $PLUGIN_DIR"
fi
if [[ "$failed" -gt 0 ]]; then
	echo "Failed to fetch $failed plugin(s); see errors above." >&2
	exit 1
fi
