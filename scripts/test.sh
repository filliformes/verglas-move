#!/bin/bash
# Native (host) build + test of the DSP wrapper. Runs in the Docker image and
# in CI (needs a native g++).
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$PROJECT_DIR/src"
OUT="$PROJECT_DIR/build/test_verglas"
mkdir -p "$PROJECT_DIR/build"
g++ -std=c++17 -O2 -ffast-math -fno-exceptions -fno-rtti -DTEST -I"$SRC_DIR" \
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-unused-local-typedefs -Wno-class-memaccess -Wno-misleading-indentation \
    "$PROJECT_DIR/tests/test_verglas.cpp" \
    "$SRC_DIR/clouds/dsp/granular_processor.cc" \
    "$SRC_DIR/clouds/dsp/correlator.cc" \
    "$SRC_DIR/clouds/dsp/mu_law.cc" \
    "$SRC_DIR/clouds/resources.cc" \
    "$SRC_DIR/clouds/dsp/pvoc/phase_vocoder.cc" \
    "$SRC_DIR/clouds/dsp/pvoc/stft.cc" \
    "$SRC_DIR/clouds/dsp/pvoc/frame_transformation.cc" \
    "$SRC_DIR/stmlib/dsp/units.cc" \
    "$SRC_DIR/stmlib/dsp/atan.cc" \
    -o "$OUT" -lm
"$OUT"
