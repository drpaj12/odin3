#!/bin/sh
# Stands in for odin3-abc: one line of 100000 bytes, then a short one.
head -c 100000 /dev/zero | tr '\000' x
echo
echo "after"
