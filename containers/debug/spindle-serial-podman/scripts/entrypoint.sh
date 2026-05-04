#!/bin/bash

# Simple entrypoint for serial container
# No Flux broker, no munged to start

# Just keep container running so we can exec into it
exec sleep infinity
