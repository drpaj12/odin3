#!/bin/sh
# Stands in for odin3-abc: dies of SIGSEGV.
echo "about to crash"
kill -SEGV $$
sleep 5
