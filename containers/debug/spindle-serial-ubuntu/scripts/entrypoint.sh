#!/bin/bash
set -e

# Start munged in background with verbose output
echo "Starting munged..."
echo "Munge key permissions:"
ls -la /etc/munge/munge.key
echo "Run directory permissions:"
ls -ld /run/munge

# Try to start munged and capture any errors
if ! sudo -u munge /usr/sbin/munged 2>&1; then
    echo "ERROR: munged failed to start"
    echo "Checking munged logs:"
    journalctl -u munge 2>/dev/null || echo "No systemd journal available"
    exit 1
fi

# Give munged a moment to create socket
sleep 1

# Check if munged process is actually running
echo "Checking munged process:"
pgrep -a munged || echo "munged process not found"

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

# Check what's in the munge directory
echo "Contents of /run/munge/:"
ls -la /run/munge/

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

