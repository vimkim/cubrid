#!/bin/sh

exec /usr/bin/time -f "$NAME: %E" \
    csql.sh -i "$1" > /dev/null

