#!/bin/bash
set -euo pipefail
if ! declare -F require_spindle_env >/dev/null; then
    printf 'Please source env.sh in this shell before running this script.\n' >&2
    exit 1
fi
check_spindle_tag || exit 1

export DEBUG=1
export VERBOSE=1
export V=1

cd "$SPINDLE_FLUX_BUILD" 
make -j 2>&1 | ts 'flux   %Y-%m-%d %H:%M:%S'| tee ./build.out
cd - >/dev/null 

cd "$SPINDLE_SERIAL_BUILD" 
make -j 2>&1 | ts 'serial %Y-%m-%d %H:%M:%S'| tee ./build.out
cd - >/dev/null 

cd "$SPINDLE_SLURM_BUILD" 
make -j 2>&1 | ts 'slurm  %Y-%m-%d %H:%M:%S'| tee ./build.out
cd - >/dev/null 

cd "$SPINDLE_PLUGIN_BUILD" 
make -j 2>&1 | ts 'plugin %Y-%m-%d %H:%M:%S'| tee ./build.out
cd - >/dev/null 

