#!/bin/bash

set -euxo pipefail

# Get the actual user from whoami (should be spindleuser)
ACTUAL_USER=$(whoami)

mkdir -p /home/${ACTUAL_USER}/Spindle-build
cd /home/${ACTUAL_USER}/Spindle-build
/home/${ACTUAL_USER}/Spindle/configure \
    --prefix=/home/${ACTUAL_USER}/Spindle-inst \
    --enable-sec-none \
    --with-rm=serial \
    --with-localstorage=/tmp \
    CFLAGS="-O2 -g" \
    CXXFLAGS="-O2 -g"
make -j$(nproc)
make install

# Build the testsuite
echo "Building testsuite..."
cd /home/${ACTUAL_USER}/Spindle-build/testsuite
make
