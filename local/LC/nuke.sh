#!/bin/bash
set -euo pipefail
if ! declare -F require_spindle_env >/dev/null; then
    printf 'Please source env.sh in this shell before running this script.\n' >&2
    exit 1
fi
check_spindle_tag || exit 1

printf 'Removing build and install directories for %s/%s:\n' "$SPINDLE_TAG" "$SPINDLE_RESOURCE_MANAGER"
printf '  Build:   %s\n' "$SPINDLE_BUILD"
printf '  Install: %s\n' "$SPINDLE_INSTALL"

# Only remove if they exist
if [[ -d "$SPINDLE_BUILD" ]]; then
    rm -rf "$SPINDLE_BUILD"
    printf 'Removed: %s\n' "$SPINDLE_BUILD"
else
    printf 'Not found (skipping): %s\n' "$SPINDLE_BUILD"
fi

if [[ -d "$SPINDLE_INSTALL" ]]; then
    rm -rf "$SPINDLE_INSTALL"
    printf 'Removed: %s\n' "$SPINDLE_INSTALL"
else
    printf 'Not found (skipping): %s\n' "$SPINDLE_INSTALL"
fi

printf 'Nuke complete.\n'

