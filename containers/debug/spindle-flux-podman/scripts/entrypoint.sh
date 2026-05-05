#!/bin/bash

# Simple entrypoint for flux podman container
# Starts flux broker

# Set Flux state directory
export FLUX_URI=local:///run/flux/local

# Start Flux broker with proper state directory
exec flux start --setattr=statedir=/run/flux /bin/bash -c "sleep infinity"
