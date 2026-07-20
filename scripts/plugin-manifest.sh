#!/usr/bin/env bash
# Shared parser for scripts/bundled-plugins.txt.

plugin_manifest_entries() {
	local manifest="${1:?plugin manifest path is required}"
	[[ -f "$manifest" ]] || {
		echo "Error: plugin manifest not found: $manifest" >&2
		return 1
	}

	# Remove comments and surrounding whitespace, then omit blank lines.
	sed -e 's/[[:space:]]*#.*$//' \
		-e 's/^[[:space:]]*//' \
		-e 's/[[:space:]]*$//' \
		-e '/^$/d' "$manifest"
}

parse_plugin_entry() {
	local entry="${1:?plugin manifest entry is required}"
	# Outputs are globals by design so callers can consume all parsed fields.
	# shellcheck disable=SC2034
	PLUGIN_REPO_SPEC="$entry"
	PLUGIN_DIRNAME=""
	# shellcheck disable=SC2034
	PLUGIN_BRANCH=""

	if [[ "$PLUGIN_REPO_SPEC" == *:* ]]; then
		PLUGIN_DIRNAME="${PLUGIN_REPO_SPEC##*:}"
		PLUGIN_REPO_SPEC="${PLUGIN_REPO_SPEC%%:*}"
	fi
	PLUGIN_OWNER_REPO="$PLUGIN_REPO_SPEC"
	if [[ "$PLUGIN_REPO_SPEC" == *@* ]]; then
		# shellcheck disable=SC2034
		PLUGIN_BRANCH="${PLUGIN_REPO_SPEC##*@}"
		PLUGIN_OWNER_REPO="${PLUGIN_REPO_SPEC%%@*}"
	fi
	if [[ -z "$PLUGIN_DIRNAME" ]]; then
		PLUGIN_DIRNAME="${PLUGIN_OWNER_REPO##*/}"
		PLUGIN_DIRNAME="${PLUGIN_DIRNAME#feedback-plugin-}"
		PLUGIN_DIRNAME="${PLUGIN_DIRNAME//-/_}"
	fi

	[[ "$PLUGIN_OWNER_REPO" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || {
		echo "Error: invalid plugin repository in manifest: $entry" >&2
		return 1
	}
	[[ "$PLUGIN_DIRNAME" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]*$ ]] || {
		echo "Error: invalid plugin directory in manifest: $entry" >&2
		return 1
	}
}
