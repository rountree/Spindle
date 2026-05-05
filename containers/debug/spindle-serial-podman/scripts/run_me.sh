#!/bin/bash
#
# Run Spindle tests in a single Podman container
# Rootless-compatible, serial resource manager
# LLNL-specific workarounds included
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONTAINER_DIR="$(dirname "$SCRIPT_DIR")"
REPO_ROOT="$(cd "$CONTAINER_DIR/../../.." && pwd)"

IMAGE_NAME="spindle-serial-podman:latest"
CONTAINER_NAME="spindle-serial-test"

# Color output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}==> Enabling Podman (LLNL-specific)${NC}"
enable-podman || true  # May not be needed on all systems

echo -e "${GREEN}==> Cleaning up any existing container${NC}"
# Remove container if it exists (from previous failed run)
if podman ps -a --filter "name=$CONTAINER_NAME" --format "{{.Names}}" | grep -q "$CONTAINER_NAME"; then
    echo "Found existing container, removing..."
    podman stop "$CONTAINER_NAME" 2>/dev/null || true
    podman rm "$CONTAINER_NAME" 2>/dev/null || true
fi

echo -e "${GREEN}==> Building Spindle serial Podman image${NC}"
cd "$REPO_ROOT"
podman build \
    --userns-uid-map=0:0:1 \
    --userns-uid-map=1:1:1999 \
    --userns-uid-map=65534:2000:2 \
    -t "$IMAGE_NAME" \
    -f containers/debug/spindle-serial-podman/Dockerfile \
    .

echo -e "${GREEN}==> Starting container${NC}"
podman run -d \
    --name "$CONTAINER_NAME" \
    --uidmap 0:0:2000 \
    --uidmap 65534:2000:2 \
    "$IMAGE_NAME"

# Wait for container to be ready
sleep 2

# Check if container is still running
if ! podman ps --filter "name=$CONTAINER_NAME" --format "{{.Names}}" | grep -q "$CONTAINER_NAME"; then
    echo -e "${RED}==> Container exited immediately! Checking logs:${NC}"
    podman logs "$CONTAINER_NAME"
    podman rm "$CONTAINER_NAME"
    exit 1
fi

echo -e "${GREEN}==> Running tests${NC}"
podman exec "$CONTAINER_NAME" bash -c 'cd /home/spindleuser/Spindle-build/testsuite/debug && \
    ./runTests.py \
        --run-all-tests \
        --resource-manager=serial \
        --spindle-debug=3 \
        --preserve-logs-on-success \
        --verbose'

RESULT=$?

echo -e "${GREEN}==> Collecting logs${NC}"
# Check what's in the debug directory
echo -e "${YELLOW}Debug directory contents:${NC}"
podman exec "$CONTAINER_NAME" ls -la /home/spindleuser/Spindle-build/testsuite/debug/
# Copy logs out of container
echo -e "${YELLOW}Copying logs to ./podman-logs/${NC}"
podman cp "$CONTAINER_NAME:/home/spindleuser/Spindle-build/testsuite/debug/." ./podman-logs/

echo -e "${GREEN}==> Stopping and removing container${NC}"
podman stop "$CONTAINER_NAME"
podman rm "$CONTAINER_NAME"

if [ $RESULT -eq 0 ]; then
    echo -e "${GREEN}==> Tests PASSED${NC}"
else
    echo -e "${RED}==> Tests FAILED${NC}"
    echo -e "${YELLOW}==> Check ./podman-logs/ for output${NC}"
fi

exit $RESULT
