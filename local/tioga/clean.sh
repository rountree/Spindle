#!/bin/bash

source ./env.sh
export DEBUG=1
export VERBOSE=1
export V=1

cd ${SPINDLE_BUILD}
make clean -j 2>&1 | tee ./blr.out
cd - > /dev/null

