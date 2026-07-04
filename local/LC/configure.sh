#!/bin/bash

if [[ -v SPINDLE_BUILD ]]; then
    echo $(date) "Configuring to build in " $SPINDLE_BUILD
    echo $(date) "Configuring to install in " $SPINDLE_INSTALL
else
    echo "SPINDLE_BUILD not set, please source env.sh.  Exiting."
    exit
fi

mkdir -p ${SPINDLE_BUILD}
mkdir -p ${SPINDLE_INSTALL}

cd ${SPINDLE_BUILD}

${SPINDLE_REPO}/configure                       \
    --prefix=${SPINDLE_INSTALL}                 \
    --enable-sec-munge                          \
    --with-rm=${TEST_RESOURCE_MANAGER}          \
    --with-cachepaths=/:/foo:/bar/:/tmp/commpath/cachepath:/baz   \
    --with-commpaths=/:/foo:/bar/:/tmp/commpath:/baz  \
    CFLAGS="-Wall -Wextra -Werror -O2 -g"       \
    CXXFLAGS="-Wall -Wextra -Werror -O2 -g"


#    --with-cachepaths=/:/foo:/bar/:/tmp/commpath/cachepath:/baz   \
