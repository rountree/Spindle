#!/bin/bash

# Simple entrypoint for flux podman container
# Starts flux broker directly in single-node standalone mode

# Create broker config for standalone mode
export FLUX_URI=local:///run/flux/local

# Start broker directly (not via flux start wrapper) for single-node standalone
exec flux broker --setattr=rundir=/run/flux --setattr=statedir=/run/flux \
    --setattr=local-uri=local:///run/flux/local \
    -Stbon.fanout=256 -Slog-stderr-level=6 \
    /bin/bash -c "sleep infinity"
