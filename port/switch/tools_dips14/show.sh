#!/bin/bash
# show.sh <file> <line> [n]: lines around a line
sed -n "$(( $2 - ${3:-8} )),$(( $2 + ${3:-8} ))p" "$1" | cut -c1-170
