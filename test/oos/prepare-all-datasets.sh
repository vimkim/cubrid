#!/usr/bin/env bash
set -e

ROWS=$1

if [ -z "$ROWS" ]; then
  echo "Usage: $0 <number_of_rows>"
  exit 1
fi

# Resolve the directory of this script
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Fix the working directory to the script’s location
cd "$SCRIPT_DIR" || return

echo "Running prepare-heap.sh... $ROWS rows"
time ./prepare-heap.sh "${ROWS}"
echo

echo "Running prepare-ovf.sh..."
time ./prepare-ovf.sh "${ROWS}"

