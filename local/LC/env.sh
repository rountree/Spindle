# shellcheck shell=bash
# Source this file rather than executing it, e.g.,
#
#   source env.sh <branch> <resource-manager>
#
# Where:
#   <branch>            - Name of the Spindle worktree branch to build
#   <resource-manager>  - One of: serial, flux, slurm, slurm-plugin

# WHAT THIS DOES
#   Sets up per-resource-manager, per-branch build and install directories in
#   a common space. Builds for different branches and resource managers won't
#   interfere with each other.

# ASSUMES
#   ${SPINDLE_WORKSPACE}/Spindle is the top level directory of a Spindle repo
#   set up for worktrees. Subdirectories (devel, blr-scripts, etc.) are the
#   individual worktree branches.

# CONFIGURATION
#   The only variable that should need to be changed on LC machines is
#   SPINDLE_WORKSPACE (set on line ~115 below).
#
# VARIABLES SET BY THIS SCRIPT
#   For use by configure, build, install, and other scripts:
#
#   SPINDLE_WORKSPACE           Directory that holds the Spindle install and
#                               build directories. (must be set by user)
#   SPINDLE_REPO                Spindle worktree directory for the specified
#                               branch, under SPINDLE_WORKSPACE.
#   SPINDLE_SCRIPTS             Path to the blr-scripts/local/LC directory.
#   SPINDLE_TAG                 The branch name or, if a specific commit is
#                               checked out, the short commit hash.
#   SPINDLE_RESOURCE_MANAGER    The resource manager (serial, flux, slurm,
#                               or slurm-plugin).
#   SPINDLE_BUILD               Directory where configure is run. Format:
#                               Spindle-${SPINDLE_TAG}-${SPINDLE_RESOURCE_MANAGER}
#   SPINDLE_INSTALL             Installation prefix passed to --prefix.
#                               Format: same as SPINDLE_BUILD.
#   FLUXRC                      (flux only) Path to spindle.rc file.
#   SPINDLE_FLUXOPT             (flux only) Set to 'disable'.
#   LD_LIBRARY_PATH             Modified to include Cray libraries and this
#                               Spindle installation.
#

#set -euo pipefail

#
# Helper functions.
# 
die() { printf '%s\n' "$*" >&2; }

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
export -f _get_current_spindle_tag

_set_spindle_tag() {
    SPINDLE_TAG=$(_get_current_spindle_tag) || return 1
    export SPINDLE_TAG
}

# This is used by other scripts.
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

# This is used by other scripts.
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

# Helper to list valid branches from worktree
_list_valid_branches() {
    local worktree_base="${SPINDLE_WORKSPACE}/Spindle"
    local bare_repo="${worktree_base}/Spindle.git"

    if [[ ! -d "$bare_repo" ]]; then
        printf "Cannot find bare repository at: %s\n" "$bare_repo" >&2
        return 1
    fi

    # Use git worktree list from the bare repo to get actual worktree branches
    git -C "$bare_repo" worktree list --porcelain 2>/dev/null | \
        awk '/^branch/ {sub(/^branch refs\/heads\//, ""); print}' | \
        sort
}

# Make this a function so we can use return on error instead of exit.
check_environment() {
    local branch resource_manager valid_branches

    #
    # Test to make sure this file was sourced rather than executed.
    #
    [[ ${BASH_SOURCE[0]} != "$0" ]] || {
        die "This file must be sourced, not executed."
        return 1
    }

    #
    # Make sure we were given two parameters: branch and resource-manager.
    #
    if [[ -z "${1-}" ]] || [[ -z "${2-}" ]]; then
        die "usage: source env.sh <branch> <resource-manager>"
        die "  where <resource-manager> is one of: serial, flux, slurm, slurm-plugin"
        return 1
    fi

    branch="$1"
    resource_manager="$2"

    #
    # SPINDLE_WORKSPACE should be the only variable that needs to change on relocation.
    #
    export SPINDLE_WORKSPACE="/p/vast1/${USER}/${LCSCHEDCLUSTER}/sandbox/workspace-Spindle"
    if [[ ! -d "$SPINDLE_WORKSPACE" ]]; then
        die "SPINDLE_WORKSPACE=${SPINDLE_WORKSPACE} is not a directory."
        die "Please edit env.sh to set the correct path."
        return 1
    fi

    #
    # Validate resource manager parameter.
    #
    case "$resource_manager" in
        serial|flux|slurm|slurm-plugin)
            export SPINDLE_RESOURCE_MANAGER="$resource_manager"
            ;;
        *)
            die "Invalid resource manager: ${resource_manager}"
            die "Valid options: serial, flux, slurm, slurm-plugin"
            return 1
            ;;
    esac

    #
    # SPINDLE_REPO is the worktree for the specified branch.
    #
    export SPINDLE_REPO="${SPINDLE_WORKSPACE}/Spindle/${branch}"

    # Validate the branch exists and is a valid worktree
    if [[ ! -d "$SPINDLE_REPO" ]]; then
        die "Branch '${branch}' not found at: ${SPINDLE_REPO}"
        printf "Valid branches:\n" >&2
        valid_branches=$(_list_valid_branches)
        if [[ -n "$valid_branches" ]]; then
            printf "  %s\n" $valid_branches >&2
        else
            printf "  (Unable to list branches - check your worktree setup)\n" >&2
        fi
        return 1
    fi

    # Make sure it's a git repository
    if ! git -C "${SPINDLE_REPO}" rev-parse --git-dir >/dev/null 2>&1; then
        die "SPINDLE_REPO=${SPINDLE_REPO} is not a valid git repository."
        return 1
    fi

    #
    # SPINDLE_SCRIPTS points to the blr-scripts worktree.
    #
    export SPINDLE_SCRIPTS="${SPINDLE_WORKSPACE}/Spindle/blr-scripts/local/LC"
    if [[ ! -d "$SPINDLE_SCRIPTS" ]]; then
        die "SPINDLE_SCRIPTS=${SPINDLE_SCRIPTS} is not a directory."
        die "Make sure the blr-scripts worktree exists."
        return 1
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
        { die "Error loading modules.  Bye."; return 1; }
    fi

    # This is not readonly, as it will be rewritten if the script is re-sourced
    #   after switching to a different branch.
    export SPINDLE_TAG=""


    _set_spindle_tag || { die "Difficulty setting SPINDLE_TAG"; return 1; }

    #
    # Set build and install directories based on branch and resource manager.
    # Format: Spindle-${SPINDLE_TAG}-${SPINDLE_RESOURCE_MANAGER}
    #
    export SPINDLE_BUILD="${SPINDLE_WORKSPACE}/build/Spindle-${SPINDLE_TAG}-${SPINDLE_RESOURCE_MANAGER}"
    export SPINDLE_INSTALL="${SPINDLE_WORKSPACE}/install/Spindle-${SPINDLE_TAG}-${SPINDLE_RESOURCE_MANAGER}"

    # Set Spindle log level (max=3)
    export SPINDLE_DEBUG=3

    #
    # Resource-manager-specific settings
    #
    if [[ "$SPINDLE_RESOURCE_MANAGER" == "flux" ]]; then
        # Tell flux what we're doing.
        export FLUXRC="${SPINDLE_BUILD}/testsuite/spindle.rc"

        # Prevents flux from using the system spindle.
        export SPINDLE_FLUXOPT=disable
    else
        # Unset flux-specific variables for non-flux resource managers
        unset FLUXRC
        unset SPINDLE_FLUXOPT
    fi

    # Allows test scripts to be able to find libmodules.so.1
    #   (Extra magic for nice colon placement in corner cases.)
    export LD_LIBRARY_PATH="/opt/cray/pe/cce/18.0.1/cce/x86_64/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    # Points to our version of Spindle
    export LD_LIBRARY_PATH="${SPINDLE_INSTALL}/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    #
    # Display configured environment
    #
    printf '\nSpindle environment configured:\n' >&2
    printf '  Branch:           %s\n' "$SPINDLE_TAG" >&2
    printf '  Resource Manager: %s\n' "$SPINDLE_RESOURCE_MANAGER" >&2
    printf '  Build:            %s\n' "$SPINDLE_BUILD" >&2
    printf '  Install:          %s\n' "$SPINDLE_INSTALL" >&2
    if [[ "$SPINDLE_RESOURCE_MANAGER" == "flux" ]]; then
        printf '  FLUXRC:           %s\n' "$FLUXRC" >&2
    fi
    printf '\n' >&2
}

check_environment "$@"

