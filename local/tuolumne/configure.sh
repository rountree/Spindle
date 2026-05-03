#!/bin/bash

if [[ -v SPINDLE_SCRIPTS ]]; then
    echo "Using scripts in " $SPINDLE_SCRIPTS
else
    echo "SPINDLE_SCRIPTS not set, please source env.h.  Exiting."
fi

echo SPINDLE_BUILD = $SPINDLE_BUILD
echo SPINDLE_REPO  = $SPINDLE_REPO

mkdir -p ${SPINDLE_BUILD}
mkdir -p ${SPINDLE_INSTALL}

export EXTRA_CFLAGS=
export EXTRA_CXXFLAGS=
export EXTRA_CPPFLAGS=
export EXTRA_LDFLAGS=

export DUST_BUNNIES="-Wall -Wextra -Werror"

cd ${SPINDLE_BUILD}

CFLAGS="-g -O2 -Wall ${EXTRA_CFLAGS} ${DUST_BUNNIES}" \
    CC=${HOME}/v/machines/tuolumne/install/gcc-15.1.0/bin/gcc \
    CXX=${HOME}/v/machines/tuolumne/install/gcc-15.1.0/bin/g++ \
	CXXFLAGS="-g -O2 ${EXTRA_CXXFLAGS} ${DUST_BUNNIES}" \
	CPPFLAGS="-g -O2 ${EXTRA_CPPFLAGS} ${DUST_BUNNIES}" \
    LDFLAGS="${EXTRA_LDFLAGS}"      \
	MPICC=`which mpicc`			    \
	MPICXX=`which mpicxx`			\
    CC=`which gcc`                  \
    CXX=`which g++`                 \
	${SPINDLE_REPO}/configure		\
 	--prefix=${SPINDLE_INSTALL}		\
 	--enable-sec-munge			    \
  	--enable-flux-plugin            \
    --enable-slurm-plugin           \
    --with-cachepaths=/tmp          \
    --with-commpath=/tmp            \
    --with-rm=serial





