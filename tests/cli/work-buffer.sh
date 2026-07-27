#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1

# Grow far enough that the relocated heap prefix is much larger than the
# library's fixed 1 MiB transfer buffer. The sparse backing file keeps the test
# cheap while proving relocation is streamed rather than staged in memory.
INITIAL_SECTORS=1048576 \
BACKING_SECTORS=41943040 \
CLUSTER_SIZE=512 \
MIN_HEAP_SHIFT_CLUSTERS=262145 \
    "$(dirname "$0")/preserve-data.sh" "$program"
echo "work-buffer: passed"
