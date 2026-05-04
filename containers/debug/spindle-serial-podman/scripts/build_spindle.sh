#!/bin/bash

set -euxo pipefail

mkdir -p /home/${USER}/Spindle-build
cd /home/${USER}/Spindle-build
/home/${USER}/Spindle/configure \
    --prefix=/home/${USER}/Spindle-inst \
    --enable-sec-none \
    --with-rm=serial \
    --with-localstorage=/tmp \
    CFLAGS="-O2 -g" \
    CXXFLAGS="-O2 -g"
make -j$(nproc)
make install
