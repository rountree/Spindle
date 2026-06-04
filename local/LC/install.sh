#!/bin/bash
if [[ -v SPINDLE_INSTALL ]]; then
    echo $(date) "Installing into " $SPINDLE_INSTALL
else
    echo "SPINDLE_INSTALL not set, please source env.sh.  Exiting."
    exit
fi


export DEBUG=1
export VERBOSE=1
export V=1

cd ${SPINDLE_BUILD}
make install -j 2>&1 | tee ./install.out
cd - > /dev/null

