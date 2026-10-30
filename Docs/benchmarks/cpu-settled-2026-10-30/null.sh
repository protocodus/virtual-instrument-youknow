#!/bin/bash
# null.sh NULLTEST_BIN OUTDIR -- render all 144 presets in four parallel shards
set -e
BIN=$1; OUT=$2; mkdir -p "$OUT"
pids=()
for r in "0 35" "36 71" "72 107" "108 143"; do "$BIN" "$OUT" original 48000 normal $r > "$OUT/log.${r% *}" 2>&1 & pids+=($!); done
for pid in "${pids[@]}"; do wait "$pid"; done
ls "$OUT"/*.f32 | wc -l
