#!/bin/bash
set -euo pipefail
if ! declare -F require_spindle_env >/dev/null; then
    printf 'Please source env.sh in this shell before running this script.\n' >&2
    exit 1
fi
check_spindle_tag || exit 1

if [[ ! -d "${SPINDLE_BUILD}" ]]; then
    printf 'Build directory does not exist: %s\n' "${SPINDLE_BUILD}" >&2
    printf 'Please run configure.sh and build.sh first.\n' >&2
    exit 1
fi

export DEBUG=1
export VERBOSE=1
export V=1

printf '%(%F %T)T Installing %s from %s to %s\n' -1 "$SPINDLE_RESOURCE_MANAGER" "$SPINDLE_BUILD" "$SPINDLE_INSTALL"

cd "$SPINDLE_BUILD"
make install -j 2>&1 | ts "${SPINDLE_RESOURCE_MANAGER} %Y-%m-%d %H:%M:%S" | tee ./install.out
cd - >/dev/null

printf '%(%F %T)T Installation complete\n' -1 
