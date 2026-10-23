#!/bin/bash
# paired_timing.sh BASE CAND OUT.csv [REPS] [BLOCKS] -- interleaved baseline/candidate runs of the bench binaries
# rows: rate,circuit,preset,rep,order,base_median_us,cand_median_us
BASE=$1; CAND=$2; OUT=$3; REPS=${4:-5}; BLOCKS=${5:-300}
echo "rate,circuit,preset,rep,first,base_median_us,cand_median_us,base_hash,cand_hash" > "$OUT"
for cfg in "48000 normal 0" "48000 normal 40" "48000 normal 80" "48000 normal 130" "44100 normal 0" "96000 normal 0" "48000 max 0" "48000 max 80"; do
  set -- $cfg; rate=$1; circ=$2; preset=$3
  for rep in $(seq 1 $REPS); do
    if [ $((rep % 2)) -eq 1 ]; then first=base; b=$($BASE original $rate $circ $BLOCKS 2 1 $preset); c=$($CAND original $rate $circ $BLOCKS 2 1 $preset);
    else first=cand; c=$($CAND original $rate $circ $BLOCKS 2 1 $preset); b=$($BASE original $rate $circ $BLOCKS 2 1 $preset); fi
    bm=$(echo "$b" | grep -o "median_us [0-9.]*" | cut -d' ' -f2); cm=$(echo "$c" | grep -o "median_us [0-9.]*" | cut -d' ' -f2)
    bh=$(echo "$b" | grep -o "hash [0-9a-f]*" | cut -d' ' -f2); ch=$(echo "$c" | grep -o "hash [0-9a-f]*" | cut -d' ' -f2)
    echo "$rate,$circ,$preset,$rep,$first,$bm,$cm,$bh,$ch" >> "$OUT"
  done
done
echo "written $OUT"
