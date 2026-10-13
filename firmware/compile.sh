#!/usr/bin/env bash
# Compile firmware/grokdesk for ESP32 with arduino-cli.
# TFT_eSPI pin defines match firmware/grokdesk/build_opt.h (Wokwi + discrete wiring).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SKETCH="$ROOT/firmware/grokdesk"
OUT="$SKETCH/build"
FLAGS="$(grep -v '^$' "$SKETCH/build_opt.h" | tr '\n' ' ')"
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --build-property "compiler.cpp.extra_flags=${FLAGS}" \
  --build-property "compiler.c.extra_flags=${FLAGS}" \
  --output-dir "$OUT" \
  "$SKETCH"
echo "firmware: $OUT/grokdesk.ino.bin"
echo "elf:      $OUT/grokdesk.ino.elf"
