#!/bin/bash
#
# trigger-ci.sh - Trigger GitHub Actions CI workflow from command line
#
# Usage:
#   ./trigger-ci.sh [options]
#
# Options:
#   --branch BRANCH    Branch to run CI on (default: current branch)
#   --ref REF          Specific git ref to run on
#   --watch            Watch the run in real-time
#   --wait             Wait for completion
#   --download         Download artifacts after completion
#   --help             Show this help

set -euo pipefail

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GH_BIN="/g/g24/rountree/v/rzadams/sandbox/workspace-Spindle/install/gh-2.96.0/bin/gh"
WORKFLOW="ci.yml"

# Default options
BRANCH=""
REF=""
WATCH=false
WAIT=false
DOWNLOAD=false

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --branch)
            BRANCH="$2"
            shift 2
            ;;
        --ref)
            REF="$2"
            shift 2
            ;;
        --watch)
            WATCH=true
            shift
            ;;
        --wait)
            WAIT=true
            shift
            ;;
        --download)
            DOWNLOAD=true
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

# Determine ref
if [[ -n "$REF" ]]; then
    TARGET_REF="$REF"
elif [[ -n "$BRANCH" ]]; then
    TARGET_REF="$BRANCH"
else
    # Use current branch
    TARGET_REF=$(git branch --show-current)
fi

echo "Triggering CI workflow '$WORKFLOW' on ref: $TARGET_REF"

# Trigger the workflow
if [[ -n "$REF" ]] || [[ -n "$BRANCH" ]]; then
    "$GH_BIN" workflow run "$WORKFLOW" --ref "$TARGET_REF"
else
    "$GH_BIN" workflow run "$WORKFLOW"
fi

echo "Workflow triggered successfully."

# Watch or wait if requested
if [[ "$WATCH" == "true" ]]; then
    echo "Watching workflow run..."
    sleep 2  # Give GitHub a moment to create the run
    "$GH_BIN" run watch
elif [[ "$WAIT" == "true" ]]; then
    echo "Waiting for workflow to complete..."
    sleep 2
    RUN_ID=$("$GH_BIN" run list --workflow="$WORKFLOW" --limit 1 --json databaseId --jq '.[0].databaseId')
    "$GH_BIN" run watch "$RUN_ID" --exit-status
fi

# Download artifacts if requested
if [[ "$DOWNLOAD" == "true" ]]; then
    echo "Downloading artifacts..."
    sleep 2
    RUN_ID=$("$GH_BIN" run list --workflow="$WORKFLOW" --limit 1 --json databaseId --jq '.[0].databaseId')
    "$GH_BIN" run download "$RUN_ID"
    echo "Artifacts downloaded to: $(pwd)"
fi

echo "Done."
