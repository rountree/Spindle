#!/bin/bash
# process_corefiles.sh
# For each core file found in subdirectories matching _*, extract the
# executable path from the core file's ELF metadata and use gdb to
# generate a backtrace and register dump.

set -euo pipefail

# Use first argument as target directory, defaulting to current working directory
TARGET_DIR="${1:-$(pwd)}"

process_core() {
    local corefile="$1"
    local coredir="$(dirname "$corefile")"
    local corebase="$(basename "$corefile" .core)"

    local backtrace_file="${coredir}/${corebase}.backtrace"
    local registers_file="${coredir}/${corebase}.registers"

    printf "\nProcessing: %s\n" "$corefile"

    # Extract the execfn field from the core file's ELF notes
    local execfn
    readelf -n "$corefile"
    execfn=$(readelf -n "$corefile" 2>/dev/null \
        | grep 'execfn:' \
        | sed "s/.*execfn: '\\([^']*\\)'.*/\\1/")

    if [ -z "$execfn" ]; then
        printf "  WARNING: Could not extract execfn from %s, skipping\n" "$corefile"
        return
    fi

    printf "  Binary  : %s\n" "$execfn"
    printf "  Backtrace -> %s\n" "$backtrace_file"
    printf "  Registers -> %s\n" "$registers_file"

    if [ ! -f "$execfn" ]; then
        printf "  WARNING: Binary '%s' not found on filesystem\n" "$execfn"
    fi

    # Generate backtrace
    gdb --batch \
        --quiet \
        -ex "set pagination off" \
        -ex "thread apply all bt full" \
        "$execfn" "$corefile" \
        > "$backtrace_file" 2>&1 || true

    # Generate register dump
    gdb --batch \
        --quiet \
        -ex "set pagination off" \
        -ex "thread apply all info registers" \
        "$execfn" "$corefile" \
        > "$registers_file" 2>&1 || true
}

# Find all core files in subdirectories matching _*
found=0
echo -n "debugging:  file $(which readelf) -> "
file $(which readelf
echo -n "debugging:  readelf --version -> " $( readelf --version )

for dir in "${TARGET_DIR}"/_*/; do
    [ -d "$dir" ] || continue
    while IFS= read -r -d '' corefile; do
        process_core "$corefile"
        found=$((found + 1))
    done < <(find "$dir" -maxdepth 1 -name '*.core' -print0)
done

if [ "$found" -eq 0 ]; then
    printf "\nNo core files found in any _* subdirectory.\n"
else
    printf "\nDone. Processed %d core file(s).\n" "$found"
fi
