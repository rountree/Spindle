# Base image for Spindle testing containers
# Pre-installs Flux and all dependencies
# No node-count dependencies - completely reusable
ARG flux_sched_version=noble
FROM fluxrm/flux-sched:${flux_sched_version}
USER root

# Install all development packages needed for Spindle
RUN DEBIAN_FRONTEND="noninteractive" apt-get update \
    && apt-get -qq install -y --no-install-recommends \
        autotools-dev \
        autoconf \
        automake \
        cmake \
        git \
        python3 \
        openssh-server \
        openssh-client \
        libdb-dev \
        apt-utils \
        dnsutils \
        iputils-ping \
        python3-pip \
        libgcrypt20 \
        libgcrypt20-dev \
        gdb \
        software-properties-common \
    && rm -rf /var/lib/apt/lists/*

ARG USER=fluxuser

# Allow fluxuser to run as other users (needed for munged)
RUN sh -c "printf \"${USER} ALL=(ALL) NOPASSWD: ALL\\n\" >> /etc/sudoers"

# Create Flux directories with correct ownership
ENV STATE_DIR=/var/lib/flux
RUN mkdir -p ${STATE_DIR} /etc/flux/system /etc/flux/system/cron.d /etc/flux/config /run/flux /etc/flux/imp/conf.d && \
    chown -R ${USER}:${USER} /run/flux ${STATE_DIR}

# Create shared-logs directory for test output
RUN mkdir -p /shared-logs && \
    chown -R ${USER}:${USER} /shared-logs

USER ${USER}
WORKDIR /home/${USER}

# Environment variables for compatibility
ENV RDMAV_FORK_SAFE=1
ENV HWLOC_HIDE_ERRORS=2
