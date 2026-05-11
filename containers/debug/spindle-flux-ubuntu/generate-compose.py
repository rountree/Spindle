#!/usr/bin/env python3
"""
Generate docker-compose.yml with specified number of nodes.
"""

import sys
import os

def generate_compose(num_nodes):
    """Generate docker-compose.yml for num_nodes Flux nodes."""

    compose = f"""# {num_nodes}-node Flux cluster
# For information on running Flux in containers, see
# https://flux-framework.readthedocs.io/en/latest/tutorials/containers

# `replicas` must match the number of nodes defined in the services section
x-shared-workers:
  &workers
  replicas: {num_nodes}

# Base flux image version to use
# ${{ubuntu_version}}-${{flux_version}}-${{arch}}
x-shared-build-args: &shared-build-args
  flux_sched_version: noble-v0.48.0-amd64
  <<: *workers

# Docker prohibits copying files from outside of the build context.
# In order to be able to copy the whole repo into the container,
# we have to set the context to be the root of the repo.
# We then have to specify the path from there to the Dockerfile.
x-shared-build-context: &shared-build-context
  context: ../../..
  dockerfile: containers/debug/spindle-flux-ubuntu/Dockerfile
  args: *shared-build-args

# Name of the node that runs the Flux broker
x-shared-environment: &shared-environment
  mainHost: node-1
  FLUX_DEBUG_FLAGS: "255"
  <<: *workers

networks:
  flux:
    driver: bridge

volumes:
  shared-logs:

# Common parameters for all nodes (except build - only node-1 builds)
x-shared-node-parameters: &shared-node-parameters
  networks:
    - flux
  environment: *shared-environment
  volumes:
    - shared-logs:/shared-logs
  cap_add:
    - SYS_NICE  # Required for libnuma
    - SYS_ADMIN # Required for dmesg collection

services:
  node-1:
    <<: *shared-node-parameters
    build: *shared-build-context
    image: spindle-flux-ubuntu:latest
    hostname: node-1
    container_name: node-1
    # Check whether all the workers have registered
    # with the broker on the head node.
    healthcheck:
      test: ["CMD", "./flux_healthcheck.sh"]
      start_period: 15s
      interval: 5s
      timeout: 10s
      retries: 5
"""

    # Generate worker nodes (node-2 through node-N)
    for i in range(2, num_nodes + 1):
        compose += f"""
  node-{i}:
    <<: *shared-node-parameters
    image: spindle-flux-ubuntu:latest
    hostname: node-{i}
    container_name: node-{i}
"""

    return compose


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} NUM_NODES", file=sys.stderr)
        sys.exit(1)

    try:
        num_nodes = int(sys.argv[1])
        if num_nodes < 1:
            raise ValueError("NUM_NODES must be at least 1")
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    compose_content = generate_compose(num_nodes)
    print(compose_content, end='')
