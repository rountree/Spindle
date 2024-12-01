#!/bin/bash
if [[ "x$1" != "xspindletest" ]]; then
    echo Failed. Parameter not passed
    exit -1
fi
if [[ "x$SPINDLE_EXEC_TEST" != "xcorrect" ]]; then
    echo Failed. Environment not passed
    exit -1
fi
exec grep -q spindlens- /proc/self/maps
