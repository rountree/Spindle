FROM ghcr.io/rountree/spindle-serial-base:latest

ARG USER=spindleuser
ARG BUILD_ROOT=./containers/debug/spindle-serial-ubuntu

USER ${USER}
WORKDIR /home/${USER}
RUN mkdir -p /home/${USER}/Spindle
# Copy the Spindle repo into the container
COPY . /home/${USER}/Spindle
COPY ${BUILD_ROOT}/scripts/build_spindle.sh /home/${USER}/build_spindle.sh
RUN ./build_spindle.sh

COPY ${BUILD_ROOT}/scripts/entrypoint.sh /home/${USER}/entrypoint.sh
ENV PATH /home/${USER}/Spindle-inst/bin:$PATH

ENTRYPOINT /bin/bash ./entrypoint.sh

