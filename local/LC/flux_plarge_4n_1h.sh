#!/bin/bash
if ! declare -F require_spindle_env >/dev/null; then
    printf 'Please source env.sh in this shell before running this script.\n' >&2
    exit 1
fi
check_spindle_tag || exit 1
flux alloc --queue=plarge --time-limit=1h --nodes=1 --exclusive

