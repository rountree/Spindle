#!/bin/bash

# Simple entrypoint for flux podman container
# Starts flux broker

# Start Flux broker
flux start /bin/bash -c "sleep infinity"
