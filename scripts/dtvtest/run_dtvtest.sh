#!/bin/bash

DTVSRCDIR=$1
DTVBUILDDIR=$2
CC=$3
CFLAGS=$4

mkdir -p $DTVBUILDDIR
make -s -C $DTVBUILDDIR -f $DTVSRCDIR/Makefile SRCDIR=$DTVSRCDIR CC=$CC "CFLAGS=$CFLAGS" dtvtest
if [ $? != 0 ] ; then
    exit 0
fi
make -s -C $DTVBUILDDIR -f $DTVSRCDIR/Makefile SRCDIR=$DTVSRCDIR CC=$CC "CFLAGS=$CFLAG" run > /dev/null 2>&1 
exit $?

