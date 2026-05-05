# Spindle Serial base image
# Multi-stage build: ubuntu + common build packages + munge
ARG ubuntu_version=noble
FROM ubuntu:${ubuntu_version}
USER root

# Install common build packages (same as spindle-build-base) + munge
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
        munge \
        libmunge-dev \
 && rm -rf /var/lib/apt/lists/*

# Set locale
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 \
    LANGUAGE=en_US:en \
    LC_ALL=en_US.UTF-8

# Environment variables
ENV HWLOC_COMPONENTS=-gl

# Set up munge directories
RUN mkdir -p /run/munge && \
    chown munge:munge /run/munge && \
    chmod 0755 /run/munge

ARG USER=spindleuser
ARG UID=1001
ARG GID=1001

# Create user
RUN groupadd -f -g ${GID} ${USER} && \
    useradd -m -u ${UID} -g ${GID} -s /bin/bash ${USER}

WORKDIR /home/${USER}
USER ${USER}
