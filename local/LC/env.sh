# shellcheck shell=bash
# Source this file rather than executing it, e.g.,
#
#   source env.sh <my_resource_manager>
#
# Supported resource managers are flux, serial, slurm, and slurm-plugin.

# WHAT THIS DOES
#   Sets up per-resource-manager, per-branch/commit build and install directories in
#     a common space.  In other words, your build won't step on each other
#     just because you logged into a different machine or switched branches.


#
# The only two variables that should need to be changed on LC machines are
#   SPINDLE_WORKSPACE and SPINDLE_REPO.
#
# This script create the following variables for use by the
#   configure, build, install, and other scripts.
#
#   SPINDLE_WORKSPACE           Directory that holds the Spindle install and
#                                 build directories.  (must be set by user)
#   SPINDLE_REPO                Spindle repo directory, usually under
#                                 SPINDLE_WORKSPACE. (must be set by user)
#   SPINDLE_SCRIPTS             This directory.  (deprecated)
#   SPINDLE_TAG                 The current branch or, if a specific commit
#                                 is checked out, the short commit name.
#                                 Used in creating SPINDLE_BUILD and
#                                 SPINDLE_INSTALL.  Re-source this script if
#                                 you change branches and want to build, etc.
#   SPINDLE_BUILD               Spindle's configure script is run from this
#                                 directory.
#   SPINDLE_INSTALL             This value passed to --prefx during
#                                 configuration.
#   FLUXRC                      Path to the spindle.rc file in the build
#                                 directory.
#   LD_LIBRARY_PATH             Modified to include Cray libraries and this
#                                 installation of Spindle.
#

if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
    printf 'This file must be sourced, not executed.\n' >&2
    exit 1
fi

# Enable useful core files
ulimit -c unlimited

# As of June 30th 2026, these are the default loaded modules on tuolumne
#   plus a compatible recent version of gcc.  Spindle needs mpicc to build
#   the testsuite.  mpicc comes from cray/mpich/... .  Use module spider
#   to find the gcc version compatible with that.
#
#   Note:  cce/20.0.0 conflicts with gcc/13.3.1-magic
if ! module load \
    craype-x86-trento \
    libfabric/match_SHS \
    craype-network-ofi \
    perftools-base/25.09.0 \
    craype/2.7.35 \
    PrgEnv-cray/8.7.0 \
    flux_wrappers/0.1 \
    xpmem/2.6.5 \
    cray-libsci/25.09.0 \
    cray-mpich/9.0.1 \
    gcc/13.3.1-magic
then
    printf 'Error loading modules.  Bye.\n' >&2
    return 1
fi

# Only used for path construction (bottom of this file)
export SPINDLE_WORKSPACE="/p/vast1/${USER}/${LCSCHEDCLUSTER}/sandbox/workspace-Spindle"
export SPINDLE_REPO="${SPINDLE_WORKSPACE}/Spindle"
export SPINDLE_SCRIPTS="${SPINDLE_REPO}/local/LC"
export SPINDLE_REPO SPINDLE_SCRIPTS

# This is not readonly, as it will be rewritten if the script is re-sourced
#   after switching to a different branch.
export SPINDLE_TAG=""

# If the number of positional parameters $# evaluated as a number (($#)) is
#   zero, then set the positional arguments to what follows --, i.e., flux.
(($#)) || set -- flux

_get_current_spindle_tag() {
    local current_branch current_commit

    current_branch=$(git -C "$SPINDLE_REPO" branch --show-current) || return 1
    current_commit=$(git -C "$SPINDLE_REPO" rev-parse --short HEAD) || return 1

    if [[ -n $current_branch ]]; then
        printf '%s\n' "$current_branch"
    else
        printf '%s\n' "$current_commit"
    fi
}

_set_spindle_tag() {
    SPINDLE_TAG=$(_get_current_spindle_tag) || return 1
    export SPINDLE_TAG
}

check_spindle_tag() {
    local current_tag

    current_tag=$(_get_current_spindle_tag) || return 1

    if [[ ${SPINDLE_TAG:-} == "$current_tag" ]]; then
        return 0
    fi

    printf 'Error: SPINDLE_TAG="%s", but repository is currently at "%s"\n' \
        "${SPINDLE_TAG:-}" "$current_tag" >&2
    return 1
}

export -f check_spindle_tag
export -f _get_current_spindle_tag
# Set environment variables if we have a known cluster and resource manager.

# Note that "exit" would exit out of the shell being used to source this.
#   That would be suboptimal. Use return.
_set_spindle_tag || return 1

# Sets build, patch, and install directories
export SPINDLE_FLUX_BUILD="${SPINDLE_WORKSPACE}/build/Spindle-${SPINDLE_TAG}-flux"
export SPINDLE_FLUX_INSTALL="${SPINDLE_WORKSPACE}/install/Spindle-${SPINDLE_TAG}-flux"

export SPINDLE_SERIAL_BUILD="${SPINDLE_WORKSPACE}/build/Spindle-${SPINDLE_TAG}-serial"
export SPINDLE_SERIAL_INSTALL="${SPINDLE_WORKSPACE}/install/Spindle-${SPINDLE_TAG}-serial"

export SPINDLE_SLURM_BUILD="${SPINDLE_WORKSPACE}/build/Spindle-${SPINDLE_TAG}-slurm"
export SPINDLE_SLURM_INSTALL="${SPINDLE_WORKSPACE}/install/Spindle-${SPINDLE_TAG}-slurm"

export SPINDLE_PLUGIN_BUILD="${SPINDLE_WORKSPACE}/build/Spindle-${SPINDLE_TAG}-plugin"
export SPINDLE_PLUGIN_INSTALL="${SPINDLE_WORKSPACE}/install/Spindle-${SPINDLE_TAG}-plugin"

# Set Spindle log level (max=3)
export SPINDLE_DEBUG=3

# Tell flux what we're doing.
export FLUXRC="${SPINDLE_BUILD}/testsuite/spindle.rc"

# Prevents flux from using the system spindle.
export SPINDLE_FLUXOPT=disable

# Allows test scripts to be able to find libmodules.so.1
#   (Extra magic for nice colon placement in corner cases.)
export LD_LIBRARY_PATH="/opt/cray/pe/cce/18.0.1/cce/x86_64/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Points to our version of Spindle
export LD_LIBRARY_PATH="${SPINDLE_INSTALL}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

require_spindle_env() {
    if [[ -z ${SPINDLE_BUILD:-} ]]; then
        printf 'SPINDLE_BUILD not set, please source env.sh.\n' >&2
        return 1
    fi

    if ! declare -F check_spindle_tag >/dev/null; then
        printf 'check_spindle_tag is not available, please source env.sh in this shell.\n' >&2
        return 1
    fi
}

export -f require_spindle_env

#!/bin/bash

