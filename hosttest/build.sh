#!/usr/bin/env bash
# Build the host harness: the vendored Commodore core (cbm/) for Linux, with
# ASan. Same sources and defines as the Pico build, without the debugger.
#
#   hosttest/build.sh            -> hosttest/c64_host
#   NOASAN=1 hosttest/build.sh   faster runs (speed measurements)
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root

CFLAGS=(
  -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter
  -DEMUDBG_OFF -DM6502_HOST_HOOK -DZ80_HOST_HOOK
  -DCONFIG_Z80_TIMING_TSTATES=1 -DCONFIG_Z80_READ_SPECIAL=1
  -I cbm/core -I cbm/core/include -I cbm/m6502/include -I cbm/z80/include -I cbm/port
)
if [ -z "${NOASAN:-}" ]; then
  CFLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi

SRC=(
  hosttest/host_main.c
  cbm/core/*.c
  cbm/m6502/m6502.c
  cbm/z80/z80.c
  cbm/port/port_stubs.c
)

${CC:-cc} "${CFLAGS[@]}" "${SRC[@]}" -lm -o hosttest/c64_host
echo "built hosttest/c64_host"
