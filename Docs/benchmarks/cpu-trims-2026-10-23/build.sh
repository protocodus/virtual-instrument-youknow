#!/bin/bash
plugin] [JOBS]|plugin] [JOBS]
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
running=0
for s in $SRCS; do
  o="$OBJ/$(basename "$s" .cpp).o"
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || [ "$TREE/Source/DSP/YouKnowEngine.h" -nt "$o" ]; then
    g++ $FLAGS -I"$TREE/Source" -c "$s" -o "$o" &
    running=$((running+1))
    if [ $running -ge $JOBS ]; then wait -n; running=$((running-1)); fi
  fi
done
wait
g++ $FLAGS -I"$TREE/Source" -I"$TREE/Tools" -c "$HERE/$TOOL.cpp" -o "$OBJ/$TOOL.o"
g++ $FLAGS -o "$OUT" "$OBJ/$TOOL.o" $(for s in $SRCS; do echo "$OBJ/$(basename "$s" .cpp).o"; done)
echo "built $OUT"
