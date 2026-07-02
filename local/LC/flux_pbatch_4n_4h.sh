#!/bin/bash
if [[ -v SPINDLE_BUILD ]]; then
    echo $(date) "Building in " $SPINDLE_BUILD
else
    echo "SPINDLE_BUILD not set, please source env.sh.  Exiting."
    exit
fi
flux alloc --queue=pbatch --time-limit=4h --nodes=4 --exclusive


