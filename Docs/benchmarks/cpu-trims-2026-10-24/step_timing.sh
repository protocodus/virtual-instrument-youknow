#!/bin/bash
# step_timing.sh engine|firmware BEFORE AFTER STEP REPS >> OUT.csv
# Interleaved runs of two binaries, the order alternating each repetition.
# engine:   the 2026-10-23 bench at 48 kHz Normal, Original timing, A11 Brass
#           Set 1, 1000 measured 64-frame callbacks; column = median us/block.
# firmware: fwbench, 4000 host blocks of A-5 assigner foreground through the
#           audio bridge; column = mean us/block.
# Row: step,rep,first,before_us,after_us,before_hash,after_hash
set -euo pipefail
KIND=$1; BEFORE=$2; AFTER=$3; STEP=$4; REPS=$5
run() { if [ "$KIND" = engine ]; then "$1" original 48000 normal 1000 2 1 0; else "$1" 4000; fi; }
field() { if [ "$KIND" = engine ]; then grep -o "median_us [0-9.]*" | cut -d' ' -f2; else grep -o "mean_us [0-9.]*" | cut -d' ' -f2; fi; }
for rep in $(seq 1 "$REPS"); do
  if [ $((rep % 2)) -eq 1 ]; then first=before; a=$(run "$BEFORE"); b=$(run "$AFTER");
  else first=after; b=$(run "$AFTER"); a=$(run "$BEFORE"); fi
  at=$(echo "$a" | field); bt=$(echo "$b" | field)
  ah=$(echo "$a" | grep -o "hash [0-9a-f]*" | cut -d' ' -f2); bh=$(echo "$b" | grep -o "hash [0-9a-f]*" | cut -d' ' -f2)
  if [ -z "$at" ] || [ -z "$bt" ] || [ -z "$ah" ] || [ -z "$bh" ]; then echo "run failed: step $STEP rep $rep" >&2; exit 1; fi
  echo "$STEP,$rep,$first,$at,$bt,$ah,$bh"
done
