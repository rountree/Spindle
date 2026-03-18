#!/bin/bash

. ./env.sh

echo "Executing rm -rf ${SPINDLE_BUILD}/*"
rm -rf ${SPINDLE_BUILD}/*
echo "Executing rm -rf ${SPINDLE_INSTALL}/*"
rm -rf ${SPINDLE_INSTALL}/*


