#!/usr/bin/env bash
# Build the streaming-tts daemon.
set -euo pipefail
cd "$(dirname "$0")"

MOON="$(cd ../moonshine && pwd)"
ORT_LIB="$MOON/core/third-party/onnxruntime/lib/linux/x86_64"

g++ -std=c++17 -O2 -Wall \
  streaming-tts.cpp tts-streamer.cpp tts-benchmark.cpp pipewire-sink.cpp \
  -I. -I"$MOON/core" \
  $(pkg-config --cflags libpipewire-0.3) \
  -L"$MOON/build/core" -lmoonshine \
  $(pkg-config --libs libpipewire-0.3) -pthread \
  -Wl,-rpath,"$MOON/build/core" -Wl,-rpath,"$ORT_LIB" \
  -o streaming-tts

echo "Built: $(pwd)/streaming-tts"
echo "ORT/engine rpath baked in; run directly: ./streaming-tts --in /tmp/tts-in.pipe"
