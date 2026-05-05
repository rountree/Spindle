#!/bin/bash

# Simple entrypoint for flux podman container
# Starts flux broker in single-node standalone mode

# Start Flux broker with proper state directory in standalone mode (size=1)
exec flux start --test-size=1 -Sstatedir=/run/flux /bin/bash -c "sleep infinity"
