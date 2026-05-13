# Rootless Podman-compatible Spindle container
# Serial resource manager, no munge, no NUMA
# LLNL-specific workarounds included
ARG base_image=ubuntu:noble
FROM ${base_image}

ENV DEBIAN_FRONTEND=noninteractive

# LLNL workaround: Fix APT sandbox for rootless Podman
RUN echo 'APT::Sandbox::User root;' > /etc/apt/apt.conf.d/00-apt-sandbox

# Install dependencies (no munge, no libnuma)
RUN apt-get update \
    && apt-get -qq install -y --no-install-recommends \
        apt-utils \
        autoconf \
        automake \
        autotools-dev \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        g++ \
        gcc \
        gdb \
        git \
        libdb-dev \
        libgcrypt20 \
        libgcrypt20-dev \
        libtool \
        make \
        mpich \
        libmpich-dev \
        openssl \
        pkg-config \
        python3 \
        software-properties-common \
    && rm -rf /var/lib/apt/lists/*

# LLNL workaround: Install LLNL CA certificates
RUN curl --insecure -s https://adpkicert.llnl.gov/ADPKI_LLNLROOT.crt \
        -o - | openssl x509 -inform der | tee /usr/local/share/ca-certificates/LLNLROOT.crt \
 && curl --insecure -s https://adpkicert.llnl.gov/ADPKI-13.the-lab.llnl.gov_ADPKI-13.crt \
        -o - | openssl x509 -inform der | tee /usr/local/share/ca-certificates/ADPKI-13.crt \
 && curl --insecure -s https://adpkicert.llnl.gov/PAN-cspca.llnl.gov.crt \
        -o /usr/local/share/ca-certificates/cspca.crt \
 && update-ca-certificates

ARG USER=spindleuser
ARG UID=1500
ARG GID=1500

# Create non-root user (use -f to not fail if group exists)
RUN groupadd -f -g ${GID} ${USER} && \
    useradd -m -u ${UID} -g ${GID} -s /bin/bash ${USER}

# Build Spindle
WORKDIR /home/${USER}
COPY . /home/${USER}/Spindle
COPY containers/debug/spindle-serial-podman/scripts/build_spindle.sh /home/${USER}/build_spindle.sh
COPY containers/debug/spindle-serial-podman/scripts/entrypoint.sh /home/${USER}/entrypoint.sh
RUN chown -R ${USER}:${USER} /home/${USER} && \
    chmod +x /home/${USER}/entrypoint.sh

USER ${USER}
RUN ./build_spindle.sh

# Set up environment
ENV PATH=/home/${USER}/Spindle-inst/bin:${PATH}

ENTRYPOINT ["/bin/bash", "/home/spindleuser/entrypoint.sh"]
