#!/bin/bash
set -euxo pipefail

echo "Applying Podman-specific tweaks..."
chmod 644 /etc/slurm/slurm.conf
touch /etc/slurm/podman_tweaks_was_here

