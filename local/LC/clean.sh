#!/bin/bash
if [[ -v SPINDLE_BUILD ]]; then
    echo $(date) "Running make clean in " $SPINDLE_BUILD
else
    echo "SPINDLE_BUILD not set, please source env.sh.  Exiting."
    exit
fi


export DEBUG=1
export VERBOSE=1
export V=1

cd ${SPINDLE_BUILD}
make clean -j 2>&1 | tee ./clean.out
cd - > /dev/null

