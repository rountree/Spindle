#!/bin/bash
set -euxo pipefail
if ! declare -F require_spindle_env >/dev/null; then
    printf 'Please source env.sh in this shell before running this script.\n' >&2
    exit 1
fi
check_spindle_tag || exit 1

rm -rf $SPINDLE_FLUX_BUILD \
       $SPINDLE_FLUX_INSTALL \
       $SPINDLE_SERIAL_BUILD \
       $SPINDLE_SERIAL_INSTALL \
       $SPINDLE_SLURM_BUILD \
       $SPINDLE_SLURM_INSTALL \
       $SPINDLE_PLUGIN_BUILD \
       $SPINDLE_PLUGIN_INSTALL

