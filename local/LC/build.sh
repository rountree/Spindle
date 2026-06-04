#!/bin/bash
if [[ -v SPINDLE_BUILD ]]; then
    echo $(date) "Building in " $SPINDLE_BUILD
else
    echo "SPINDLE_BUILD not set, please source env.sh.  Exiting."
    exit
fi


export DEBUG=1
export VERBOSE=1
export V=1

cd ${SPINDLE_BUILD}
make -j 2>&1 | tee ./build.out
cd - > /dev/null

