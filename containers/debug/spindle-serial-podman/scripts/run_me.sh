#!/bin/bash
#
# Run Spindle tests in a single Podman container
# Rootless-compatible, serial resource manager
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

echo -e "${GREEN}==> Building Spindle serial Podman image${NC}"
cd "$REPO_ROOT"
podman build \
    -t "$IMAGE_NAME" \
    -f containers/debug/spindle-serial-podman/Dockerfile \
    .

echo -e "${GREEN}==> Starting container${NC}"
podman run -d \
    --name "$CONTAINER_NAME" \
    --rm \
    "$IMAGE_NAME"

# Wait for container to be ready
sleep 2

echo -e "${GREEN}==> Running tests${NC}"
podman exec "$CONTAINER_NAME" bash -c 'cd Spindle-build/testsuite/debug && \
    ./runTests.py \
        --resource-manager=serial \
        --spindle-debug=3 \
        --verbose'

RESULT=$?

echo -e "${GREEN}==> Collecting logs${NC}"
# Copy logs out of container if they exist
podman cp "$CONTAINER_NAME:/home/spindleuser/Spindle-build/testsuite/debug/." ./podman-logs/ 2>/dev/null || true

echo -e "${GREEN}==> Stopping container${NC}"
podman stop "$CONTAINER_NAME"

if [ $RESULT -eq 0 ]; then
    echo -e "${GREEN}==> Tests PASSED${NC}"
else
    echo -e "${RED}==> Tests FAILED${NC}"
    echo -e "${YELLOW}==> Check ./podman-logs/ for output${NC}"
fi

exit $RESULT
