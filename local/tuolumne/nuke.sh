#!/bin/bash

if [[ -v SPINDLE_SCRIPTS ]]; then
    echo "Using scripts in " $SPINDLE_SCRIPTS
else
    echo "SPINDLE_SCRIPTS not set, please source env.h.  Exiting."
fi
export DEBUG=1

echo "Executing rm -rf ${SPINDLE_BUILD}/*"
rm -rf ${SPINDLE_BUILD}/*
echo "Executing rm -rf ${SPINDLE_INSTALL}/*"
rm -rf ${SPINDLE_INSTALL}/*


