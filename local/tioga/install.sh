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
make install -j
cd - > /dev/null

