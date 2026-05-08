#!/bin/bash
set -e

# Start munged in background
echo "Starting munged..."
sudo -u munge /usr/sbin/munged

# Wait for socket to be created
echo "Waiting for munge socket..."
for i in {1..10}; do
    if [ -S /run/munge/munge.socket.2 ]; then
        echo "Munge socket created successfully"
        break
    fi
    echo "Waiting... (attempt $i/10)"
    sleep 1
done

# Verify munge is working
echo "Testing munge..."
if munge -n | unmunge; then
    echo "Munge is working correctly"
else
    echo "ERROR: Munge test failed"
    exit 1
fi

# Keep container running
echo "Container ready. Keeping alive..."
sleep infinity

