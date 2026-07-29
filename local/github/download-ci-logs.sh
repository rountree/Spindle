#!/bin/bash
#
# download-ci-logs.sh - Download logs and artifacts from GitHub Actions CI runs
#
# Usage:
#   ./download-ci-logs.sh [options]
#
# Options:
#   --run-id ID        Specific run ID to download from
#   --latest           Download from the most recent run (default)
#   --branch BRANCH    Download from latest run on specific branch
#   --output-dir DIR   Directory to save logs (default: ./ci-logs-<timestamp>)
#   --artifacts-only   Only download artifacts, not logs
#   --logs-only        Only download logs, not artifacts
#   --help             Show this help

set -euo pipefail

# Configuration
GH_BIN="/g/g24/rountree/v/rzadams/sandbox/workspace-Spindle/install/gh-2.96.0/bin/gh"
WORKFLOW="ci.yml"

# Default options
RUN_ID=""
BRANCH=""
OUTPUT_DIR=""
ARTIFACTS_ONLY=false
LOGS_ONLY=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --run-id)
            RUN_ID="$2"
            shift 2
            ;;
        --latest)
            # This is the default, just consume the flag
            shift
            ;;
        --branch)
            BRANCH="$2"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --artifacts-only)
            ARTIFACTS_ONLY=true
            shift
            ;;
        --logs-only)
            LOGS_ONLY=true
            shift
            ;;
        --help)
            grep "^#" "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Check gh is authenticated
if ! "$GH_BIN" auth status &>/dev/null; then
    echo "Error: gh is not authenticated."
    echo "Run: $GH_BIN auth login"
    exit 1
fi

# Determine run ID if not provided
if [[ -z "$RUN_ID" ]]; then
    echo "Finding latest CI run..."
    if [[ -n "$BRANCH" ]]; then
        RUN_ID=$("$GH_BIN" run list --workflow="$WORKFLOW" --branch="$BRANCH" --limit 1 --json databaseId --jq '.[0].databaseId')
    else
        RUN_ID=$("$GH_BIN" run list --workflow="$WORKFLOW" --limit 1 --json databaseId --jq '.[0].databaseId')
    fi

    if [[ -z "$RUN_ID" ]] || [[ "$RUN_ID" == "null" ]]; then
        echo "Error: No CI runs found."
        exit 1
    fi

    echo "Using run ID: $RUN_ID"
fi

# Set up output directory
if [[ -z "$OUTPUT_DIR" ]]; then
    TIMESTAMP=$(date +%Y%m%d-%H%M%S)
    OUTPUT_DIR="./ci-logs-${TIMESTAMP}-run-${RUN_ID}"
fi

mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"

echo "Downloading to: $(pwd)"

# Get run information
echo ""
echo "Run information:"
"$GH_BIN" run view "$RUN_ID"

# Download logs
if [[ "$ARTIFACTS_ONLY" == "false" ]]; then
    echo ""
    echo "Downloading logs..."
    "$GH_BIN" run view "$RUN_ID" --log > run-${RUN_ID}.log
    echo "Logs saved to: run-${RUN_ID}.log"
fi

# Download artifacts
if [[ "$LOGS_ONLY" == "false" ]]; then
    echo ""
    echo "Checking for artifacts..."
    ARTIFACT_COUNT=$("$GH_BIN" run view "$RUN_ID" --json artifacts --jq '.artifacts | length')

    if [[ "$ARTIFACT_COUNT" -gt 0 ]]; then
        echo "Downloading $ARTIFACT_COUNT artifact(s)..."
        "$GH_BIN" run download "$RUN_ID"
        echo "Artifacts downloaded."
    else
        echo "No artifacts found for this run."
    fi
fi

echo ""
echo "Download complete. Files are in: $(pwd)"
echo ""
echo "Contents:"
ls -lh
