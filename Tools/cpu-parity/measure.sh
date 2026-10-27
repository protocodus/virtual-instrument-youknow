#!/bin/bash
# measure.sh BENCH OUT.csv -- the round's callgrind workload, one row per
# configuration: workload,ir_per_callback,hash. Normal Circuit Rate on A11,
# A61, B31 and YB3 (300 callbacks), Max on A11 (200), 48 kHz, see bench.cpp.
set -euo pipefail
BIN=$1; OUT=$2; : > "$OUT"
for w in "normal 300 0" "normal 300 40" "normal 300 80" "normal 300 130" "max 200 0"; do
  set -- $w
  log=$(valgrind --tool=callgrind --collect-atstart=no --callgrind-out-file=/dev/null "$BIN" original 48000 $1 $2 2 1 $3 2>&1)
  ir=$(echo "$log" | sed -n 's/.*Collected : \([0-9,]*\).*/\1/p' | tr -d ,)
  h=$(echo "$log" | sed -n 's/^hash \([0-9a-f]*\).*/\1/p')
  if [ -z "$ir" ] || [ -z "$h" ]; then echo "run failed: $w" >&2; exit 1; fi
  echo "$1-p$3,$((ir / $2)),$h" >> "$OUT"
done
cat "$OUT"
