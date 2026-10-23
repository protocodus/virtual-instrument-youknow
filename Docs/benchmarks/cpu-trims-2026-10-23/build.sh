#!/bin/bash
# build.sh TREE bench|nulltest OUT [rack|plugin] [JOBS] -- compile one harness tool against a checkout
# rack:   C++17, -O3 -fno-builtin -DYOUKNOW_EMBEDDED_TARGET=1, engine TUs only (no Panel/Presets)
# plugin: C++20, -O3, every DSP TU (Presets available)
set -e
TREE=$1; TOOL=$2; OUT=$3; PROFILE=${4:-plugin}; JOBS=${5:-2}
HERE=$(cd "$(dirname "$0")" && pwd)
OBJ=$OUT.objs; mkdir -p "$OBJ"
if [ "$PROFILE" = rack ]; then
  FLAGS="-std=c++17 -O3 -g -fno-builtin -DNDEBUG -DYOUKNOW_EMBEDDED_TARGET=1 $EXTRA_FLAGS"
  SRCS=$(ls "$TREE"/Source/DSP/*.cpp)
else
  FLAGS="-std=c++20 -O3 -g -DNDEBUG $EXTRA_FLAGS"
  SRCS=$(ls "$TREE"/Source/DSP/*.cpp)
fi
# Every translation unit is compiled on every run: the DSP headers are part
# of what is measured, a timestamp test on the .cpp alone let a header-only
# change miss the objects, and a stale object once produced a false result.
# The previous object is removed first and every compilation is waited for
# by pid (a bare `wait` always returns 0), so a failed compile stops the
# script under set -e instead of linking an old object.
pids=()
wait_batch() { local pid; for pid in "${pids[@]}"; do wait "$pid"; done; pids=(); }
for s in $SRCS; do
  o="$OBJ/$(basename "$s" .cpp).o"
  rm -f "$o"
  g++ $FLAGS -I"$TREE/Source" -c "$s" -o "$o" &
  pids+=($!)
  if [ ${#pids[@]} -ge $JOBS ]; then wait_batch; fi
done
wait_batch
g++ $FLAGS -I"$TREE/Source" -I"$TREE/Tools" -c "$HERE/$TOOL.cpp" -o "$OBJ/$TOOL.o"
g++ $FLAGS -o "$OUT" "$OBJ/$TOOL.o" $(for s in $SRCS; do echo "$OBJ/$(basename "$s" .cpp).o"; done)
echo "built $OUT"
