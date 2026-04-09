#!/bin/bash

if [[ -v SPINDLE_SCRIPTS ]]; then
    echo "Using scripts in " $SPINDLE_SCRIPTS
else
    echo "SPINDLE_SCRIPTS not set, please source env.h.  Exiting."
fi

echo SPINDLE_BUILD = $SPINDLE_BUILD
echo SPINDLE_REPO  = $SPINDLE_REPO

mkdir -p ${SPINDLE_BUILD}
mkdir -p ${SPINDLE_INSTALL}


cd ${SPINDLE_BUILD}

${SPINDLE_REPO}/configure                       \
    --prefix=${SPINDLE_INSTALL}                 \
    --enable-sec-munge                          \
    --with-rm=${TEST_RESOURCE_MANAGER}          \
    --with-cachepaths=/tmp/commpath/cachepath   \
    --with-commpath=/tmp/commpath               \
    CFLAGS="-O2 -g"                             \
    CXXFLAGS="-O2 -g"


