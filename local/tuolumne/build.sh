#!/bin/bash

if [[ -v SPINDLE_SCRIPTS ]]; then
    echo "Using scripts in " $SPINDLE_SCRIPTS
else
    echo "SPINDLE_SCRIPTS not set, please source env.h.  Exiting."
fi

export DEBUG=1
export VERBOSE=1
export V=1

cd ${SPINDLE_BUILD}
make -j 2>&1 | tee ./blr.out
cd - > /dev/null

