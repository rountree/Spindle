#!/bin/bash
if ! declare -F require_spindle_env >/dev/null; then
    printf 'Please source env.sh in this shell before running this script.\n' >&2
    exit 1
fi
check_spindle_tag || exit 1

export DEBUG=1
export VERBOSE=1
export V=1

cd "$SPINDLE_BUILD" || exit 1
make clean -j 2>&1 | tee ./clean.out
cd - >/dev/null || exit 1

