#!/bin/bash

# Start munged in background
echo "Starting munged..."
sudo -u munge /usr/sbin/munged

# Keep container running
echo "Container ready. Keeping alive..."
sleep infinity

