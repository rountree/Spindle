#!/bin/bash
#
# Clean up existing Spindle Flux Podman container and image
# Run this before ./run_me.sh to force a full rebuild
#

set -e

CONTAINER_NAME="spindle-flux-test"
IMAGE_NAME="spindle-flux-podman:latest"

# Color output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}==> Cleaning up existing container and image${NC}"

# Stop container if running
if podman ps -q --filter "name=$CONTAINER_NAME" | grep -q .; then
    echo -e "${YELLOW}Stopping container $CONTAINER_NAME${NC}"
    podman stop "$CONTAINER_NAME"
else
    echo "Container $CONTAINER_NAME not running"
fi

# Remove container if it exists
if podman ps -a -q --filter "name=$CONTAINER_NAME" | grep -q .; then
    echo -e "${YELLOW}Removing container $CONTAINER_NAME${NC}"
    podman rm "$CONTAINER_NAME"
else
    echo "Container $CONTAINER_NAME does not exist"
fi

# Remove image if it exists
if podman images -q "$IMAGE_NAME" | grep -q .; then
    echo -e "${YELLOW}Removing image $IMAGE_NAME${NC}"
    podman rmi "$IMAGE_NAME"
else
    echo "Image $IMAGE_NAME does not exist"
fi

echo -e "${GREEN}==> Cleanup complete. Ready to rebuild.${NC}"
echo -e "${GREEN}==> Run ./run_me.sh to rebuild and test${NC}"
