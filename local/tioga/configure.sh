#!/bin/bash

source ./env.sh

mkdir -p ${SPINDLE_BUILD}
mkdir -p ${SPINDLE_INSTALL}

export EXTRA_CFLAGS=
export EXTRA_CXXFLAGS=
export EXTRA_CPPFLAGS=
export EXTRA_LDFLAGS=

export DUST_BUNNIES="-Wall -Wextra -Werror"
#export DUST_BUNNIES="      -Wextra -Werror"
#export DUST_BUNNIES="      -Wextra        "
#export DUST_BUNNIES="-Wall -Wextra        "
#export DUST_BUNNIES="-Wall         -Werror"
#export DUST_BUNNIES="-Wall                "
#export DUST_BUNNIES="              -Werror"
#export DUST_BUNNIES="                     "

cd ${SPINDLE_BUILD}

CFLAGS="-g -O2 -Wall ${EXTRA_CFLAGS} ${DUST_BUNNIES}" \
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
    --with-cachepaths=/tmp          \
    --with-commpath=/tmp            \
    --with-rm=flux


#    CC=${HOME}/v/machines/tuolumne/install/gcc-15.1.0/bin/gcc \
#    CXX=${HOME}/v/machines/tuolumne/install/gcc-15.1.0/bin/g++ \



