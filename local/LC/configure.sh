#!/bin/bash
set -euo pipefail
if ! declare -F require_spindle_env >/dev/null; then
    printf 'Please source env.sh in this shell before running this script.\n' >&2
    exit 1
fi
check_spindle_tag || exit 1

if [[ -v SPINDLE_BUILD ]]; then
    echo $(date) "Configuring to build in " $SPINDLE_BUILD
    echo $(date) "Configuring to install in " $SPINDLE_INSTALL
else
    echo "SPINDLE_BUILD not set, please source env.sh.  Exiting."
    exit
fi

mkdir -p ${SPINDLE_FLUX_BUILD}
mkdir -p ${SPINDLE_FLUX_INSTALL}
mkdir -p ${SPINDLE_SERIAL_BUILD}
mkdir -p ${SPINDLE_SERIAL_INSTALL}
mkdir -p ${SPINDLE_SLURM_BUILD}
mkdir -p ${SPINDLE_SLURM_INSTALL}
mkdir -p ${SPINDLE_PLUGIN_BUILD}
mkdir -p ${SPINDLE_PLUGIN_INSTALL}

printf "%(%F %T)T Starting flux configuration." -1
cd ${SPINDLE_FLUX_BUILD}
${SPINDLE_REPO}/configure                       \
    --prefix=${SPINDLE_FLUX_INSTALL}            \
    --enable-sec-munge                          \
    --with-rm=flux                              \
    --with-cachepaths=/:/foo:/bar/:/tmp/commpath/cachepath:/baz   \
    --with-commpaths=/:/foo:/bar/:/tmp/commpath/commpath:/baz   \
    CFLAGS="-Wall -Wextra -Werror -O2 -g"       \
    CXXFLAGS="-Wall -Wextra -Werror -O2 -g"     \
    | ts 'flux   %Y-%m-%d %H:%M:%S'
cd - > /dev/null 2>&1

printf "%(%F %T)T Starting serial configuration." -1
cd ${SPINDLE_SERIAL_BUILD}
${SPINDLE_REPO}/configure                       \
    --prefix=${SPINDLE_SERIAL_INSTALL}          \
    --enable-sec-munge                          \
    --with-rm=serial                            \
    --with-cachepaths=/:/foo:/bar/:/tmp/commpath/cachepath:/baz   \
    --with-commpaths=/:/foo:/bar/:/tmp/commpath/commpath:/baz   \
    CFLAGS="-Wall -Wextra -Werror -O2 -g"       \
    CXXFLAGS="-Wall -Wextra -Werror -O2 -g"     \
    | ts 'serial %Y-%m-%d %H:%M:%S'
cd - > /dev/null 2>&1

printf "%(%F %T)T Starting slurm configuration." -1
cd ${SPINDLE_SLURM_BUILD}
${SPINDLE_REPO}/configure                       \
    --prefix=${SPINDLE_SLURM_INSTALL}           \
    --enable-sec-munge                          \
    --with-rm=slurm                             \
    --with-rsh-launch                           \
    --with-rsh-command=/usr/bin/ssh             \
    --with-cachepaths=/:/foo:/bar/:/tmp/commpath/cachepath:/baz   \
    --with-commpaths=/:/foo:/bar/:/tmp/commpath/commpath:/baz   \
    CFLAGS="-Wall -Wextra -Werror -O2 -g"       \
    CXXFLAGS="-Wall -Wextra -Werror -O2 -g"     \
    | ts 'slurm  %Y-%m-%d %H:%M:%S'
cd - > /dev/null 2>&1

printf "%(%F %T)T Starting plugin configuration." -1
cd ${SPINDLE_PLUGIN_BUILD}
${SPINDLE_REPO}/configure                       \
    --prefix=${SPINDLE_PLUGIN_INSTALL}          \
    --enable-sec-munge                          \
    --with-rm=slurm-plugin                      \
    --with-cachepaths=/:/foo:/bar/:/tmp/commpath/cachepath:/baz   \
    --with-commpaths=/:/foo:/bar/:/tmp/commpath/commpath:/baz   \
    CFLAGS="-Wall -Wextra -Werror -O2 -g"       \
    CXXFLAGS="-Wall -Wextra -Werror -O2 -g"     \
    | ts 'plugin %Y-%m-%d %H:%M:%S'
cd - > /dev/null 2>&1

