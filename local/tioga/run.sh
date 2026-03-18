#!/bin/bash

# REMEMBER:  run with either "-o spindle" or a spindle variable like
#            "-o psindle.cachepaths=/foo/bar/baz", but not both.

. ./env.sh

echo FLUXRC=${FLUXRC}
echo SPINDLE_FLUXOPT=${SPINDLE_FLUXOPT}
echo SPINDLE_BRANCH=${SPINDLE_BRANCH}
export SPINDLE_TEST=1
flux run -o userrc=${FLUXRC} -o spindle --tasks-per-node=3 --nodes=3 /bin/true

