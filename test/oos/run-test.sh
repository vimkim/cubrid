#!/bin/sh

echo "Running test: $1"
exec /usr/bin/time -f "$NAME: %E" \
    csql.sh -i "$1" > /dev/null
echo

