#!/bin/sh
# Stands in for a runaway ABC command: records its pid in spin.pid (the working directory), then
# burns CPU until it is killed.
echo $$ > spin.pid
exec yes > /dev/null
