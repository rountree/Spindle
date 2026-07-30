#!/bin/bash
set -euo pipefail
if ! declare -F require_spindle_env >/dev/null; then
    printf 'Please source env.sh in this shell before running this script.\n' >&2
    exit 1
fi
check_spindle_tag || exit 1

if [[ ! -v SPINDLE_BUILD ]]; then
    printf 'SPINDLE_BUILD not set, please source env.sh. Exiting.\n' >&2
    exit 1
fi

printf '%(%F %T)T Configuring %s build in %s\n' -1 "$SPINDLE_RESOURCE_MANAGER" "$SPINDLE_BUILD"
printf '%(%F %T)T Will install to %s\n' -1 "$SPINDLE_INSTALL"

MY_CACHEPATHS=/tmp2:/tmp/spindle/cachepath
MY_COMMPATHS=/:/tmp/spindle/commpath

# Create build and install directories
mkdir -p "${SPINDLE_BUILD}"
mkdir -p "${SPINDLE_INSTALL}"

# Base configure options (common to all resource managers)
configure_opts=(
    "--prefix=${SPINDLE_INSTALL}"
    "--enable-sec-munge"
    "--with-rm=${SPINDLE_RESOURCE_MANAGER}"
    "--with-cachepaths=${MY_CACHEPATHS}"
    "--with-commpaths=${MY_COMMPATHS}"
    "CFLAGS=-Wall -Wextra -Werror -O2 -g"
    "CXXFLAGS=-Wall -Wextra -Werror -O2 -g"
)

# Add resource-manager-specific options
case "${SPINDLE_RESOURCE_MANAGER}" in
    slurm)
        configure_opts+=(
            "--with-rsh-launch"
            "--with-rsh-cmd=/usr/bin/ssh"
        )
        ;;
    slurm-plugin)
        configure_opts+=(
            "--enable-slurm-plugin"
        )
        ;;
    flux|serial)
        # No additional options needed
        ;;
esac

# Run configure
cd "${SPINDLE_BUILD}"
printf '%(%F %T)T Starting %s configuration\n' -1 "$SPINDLE_RESOURCE_MANAGER"
"${SPINDLE_REPO}/configure" "${configure_opts[@]}" 2>&1 | \
    ts "${SPINDLE_RESOURCE_MANAGER} %Y-%m-%d %H:%M:%S"
cd - > /dev/null 2>&1

printf '%(%F %T)T Configuration complete\n' -1

