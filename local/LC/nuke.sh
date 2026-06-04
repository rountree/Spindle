#!/bin/bash
if [[ -v SPINDLE_BUILD ]]; then
    echo $(date) "Removing directories " $SPINDLE_BUILD $SPINDLE_INSTALL
else
    echo "SPINDLE_BUILD not set, please source env.sh.  Exiting."
    exit
fi

rm -rf $SPINDLE_BUILD $SPINDLE_INSTALL

