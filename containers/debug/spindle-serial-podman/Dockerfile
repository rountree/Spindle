# Rootless Podman-compatible Spindle container
# Serial resource manager, no munge, no NUMA
ARG base_image=ubuntu:noble
FROM ${base_image}

# Install dependencies (no munge, no libnuma)
RUN DEBIAN_FRONTEND="noninteractive" apt-get update \
    && apt-get -qq install -y --no-install-recommends \
        autotools-dev \
        autoconf \
        automake \
        cmake \
        git \
        python3 \
        libdb-dev \
        apt-utils \
        gdb \
        software-properties-common \
    && rm -rf /var/lib/apt/lists/*

ARG USER=spindleuser
ARG UID=1000
ARG GID=1000

# Create non-root user
RUN groupadd -g ${GID} ${USER} && \
    useradd -m -u ${UID} -g ${GID} -s /bin/bash ${USER}

# Build Spindle
WORKDIR /home/${USER}
COPY . /home/${USER}/Spindle
COPY containers/debug/spindle-serial-podman/scripts/build_spindle.sh /home/${USER}/build_spindle.sh
RUN chown -R ${USER}:${USER} /home/${USER}

USER ${USER}
RUN ./build_spindle.sh

# Set up environment
ENV PATH=/home/${USER}/Spindle-inst/bin:${PATH}

COPY containers/debug/spindle-serial-podman/scripts/entrypoint.sh /home/${USER}/entrypoint.sh

ENTRYPOINT ["/bin/bash", "/home/spindleuser/entrypoint.sh"]
