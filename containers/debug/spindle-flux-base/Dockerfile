# Spindle Flux base image
# Multi-stage build: flux-sched + common build packages
ARG flux_sched_version=noble
FROM fluxrm/flux-sched:${flux_sched_version}
USER root

# Install common build packages (same as spindle-build-base)
RUN DEBIAN_FRONTEND="noninteractive" apt-get update \
 && apt-get -qq install -y --no-install-recommends \
        apt-utils \
        locales \
        ca-certificates \
        wget \
        git \
        ssh \
        sudo \
        build-essential \
        pkg-config \
        autotools-dev \
        libtool \
        autoconf \
        automake \
        cmake \
        make \
        gfortran-13 \
        gcc-13 \
        g++-13 \
        python3 \
        python3-pip \
        libdb-dev \
        libgcrypt20 \
        libgcrypt20-dev \
        libhwloc-dev \
        mpich \
        libmpich-dev \
        gdb \
        software-properties-common \
        dnsutils \
        iputils-ping \
        openssh-server \
        openssh-client \
 && rm -rf /var/lib/apt/lists/*

# Set locale
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 \
    LANGUAGE=en_US:en \
    LC_ALL=en_US.UTF-8

# Environment variables
ENV HWLOC_COMPONENTS=-gl \
    RDMAV_FORK_SAFE=1 \
    HWLOC_HIDE_ERRORS=2

ARG USER=fluxuser
WORKDIR /home/${USER}
USER ${USER}
