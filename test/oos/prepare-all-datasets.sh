#!/usr/bin/env bash
set -e

# Resolve the directory of this script
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Fix the working directory to the script’s location
cd "$SCRIPT_DIR" || return

echo "Running prepare-heap.sh..."
time ./prepare-heap.sh 1000000
echo

echo "Running prepare-ovf.sh..."
time ./prepare-ovf.sh 1000000

