#!/bin/sh
# Stands in for odin3-abc: prints its environment, its stdin and its core limit.
env
if read -r line; then echo "stdin-line:$line"; else echo "stdin-eof"; fi
echo "core:$(ulimit -c)"
echo "pwd:$(pwd)"
echo "args:$*"
