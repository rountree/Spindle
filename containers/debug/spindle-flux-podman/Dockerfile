# Rootless Podman-compatible Spindle Flux container
# Based on flux-sched, with LLNL-specific workarounds
ARG flux_sched_version=noble
FROM fluxrm/flux-sched:${flux_sched_version}
USER root

# LLNL workaround: Fix APT sandbox for rootless Podman
RUN echo 'APT::Sandbox::User root;' > /etc/apt/apt.conf.d/00-apt-sandbox

# Install dependencies
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
        openssh-server \
        openssh-client \
    && rm -rf /var/lib/apt/lists/*

# LLNL workaround: Install LLNL CA certificates
RUN curl --insecure -s https://adpkicert.llnl.gov/ADPKI_LLNLROOT.crt \
        -o - | openssl x509 -inform der | tee /usr/local/share/ca-certificates/LLNLROOT.crt \
 && curl --insecure -s https://adpkicert.llnl.gov/ADPKI-13.the-lab.llnl.gov_ADPKI-13.crt \
        -o - | openssl x509 -inform der | tee /usr/local/share/ca-certificates/ADPKI-13.crt \
 && curl --insecure -s https://adpkicert.llnl.gov/PAN-cspca.llnl.gov.crt \
        -o /usr/local/share/ca-certificates/cspca.crt \
 && update-ca-certificates

ARG USER=fluxuser
ARG UID=1500
ARG GID=1500

# Create non-root user (use -f to not fail if group exists)
RUN groupadd -f -g ${GID} ${USER} || true

# Configure Flux directories and certificates
ENV STATE_DIR=/var/lib/flux
RUN mkdir -p ${STATE_DIR} /etc/flux/system /etc/flux/system/cron.d /etc/flux/config /run/flux /mnt/curve && \
    flux keygen /mnt/curve/curve.cert && \
    flux R encode --hosts=localhost > /etc/flux/system/R && \
    chown -R ${USER}:${USER} /run/flux ${STATE_DIR} /mnt/curve

# Build Spindle
WORKDIR /home/${USER}
COPY . /home/${USER}/Spindle
COPY containers/debug/spindle-flux-podman/scripts/build_spindle.sh /home/${USER}/build_spindle.sh
COPY containers/debug/spindle-flux-podman/scripts/entrypoint.sh /home/${USER}/entrypoint.sh
RUN chown -R ${USER}:${USER} /home/${USER} && \
    chmod +x /home/${USER}/entrypoint.sh

USER ${USER}
RUN ./build_spindle.sh

# Set up environment
ENV PATH=/home/${USER}/Spindle-inst/bin:${PATH}
ENV RDMAV_FORK_SAFE=1
ENV HWLOC_HIDE_ERRORS=2

ENTRYPOINT ["/bin/bash", "/home/fluxuser/entrypoint.sh"]
