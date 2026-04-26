#!/bin/bash

set -euo pipefail

# Default to 4 nodes if not specified
NODE_COUNT=${1:-4}

# Validate NODE_COUNT is a positive integer
if ! [[ "$NODE_COUNT" =~ ^[0-9]+$ ]] || [ "$NODE_COUNT" -lt 1 ]; then
    echo "Error: NODE_COUNT must be a positive integer" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEMPLATE_FILE="${SCRIPT_DIR}/docker-compose.yml.template"
OUTPUT_FILE="${SCRIPT_DIR}/docker-compose.yml"

if [ ! -f "$TEMPLATE_FILE" ]; then
    echo "Error: Template file not found: $TEMPLATE_FILE" >&2
    exit 1
fi

echo "Generating docker-compose.yml for ${NODE_COUNT} nodes..."

# Generate the node services section
NODE_SERVICES=""
for i in $(seq 2 "$NODE_COUNT"); do
    NODE_SERVICES+="  node-${i}:
    <<: *shared-node-parameters
    hostname: node-${i}
    container_name: node-${i}

"
done

# Read template and replace placeholders
TEMPLATE_CONTENT=$(cat "$TEMPLATE_FILE")
TEMPLATE_CONTENT="${TEMPLATE_CONTENT//__NODE_COUNT__/$NODE_COUNT}"
TEMPLATE_CONTENT="${TEMPLATE_CONTENT//__NODE_SERVICES__/$NODE_SERVICES}"

# Write output file
echo "$TEMPLATE_CONTENT" > "$OUTPUT_FILE"

echo "Generated docker-compose.yml with ${NODE_COUNT} nodes"
