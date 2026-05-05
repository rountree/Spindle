# Common base image for all Spindle containers
# Contains expensive package installations that are shared across all RMs
# Saves ~10 minutes per build by caching this layer
ARG ubuntu_version=noble
FROM ubuntu:${ubuntu_version}
USER root

# Install apt-utils first (recommended practice)
RUN DEBIAN_FRONTEND="noninteractive" apt-get update \
 && apt-get -qq install -y --no-install-recommends \
        apt-utils

# Install all common build tools and development packages
# These are needed by all Spindle builds regardless of RM
RUN DEBIAN_FRONTEND="noninteractive" apt-get -qq install -y --no-install-recommends \
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

# Prevent hwloc from trying to use graphics cards
ENV HWLOC_COMPONENTS=-gl
