#!/bin/sh
# Stands in for odin3-abc: starts a grandchild that outlives it (its pid goes to grandchild.pid in
# the working directory, the run directory), then sleeps.
sleep 60 &
echo $! > grandchild.pid
echo "sleeping"
sleep 60
