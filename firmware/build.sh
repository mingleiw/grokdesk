#!/usr/bin/env bash
# Build the 4.3B firmware and a merged image (bootloader + partition table + app at 0).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="$ROOT/firmware"
OUT="$ROOT/flash"
cd "$FW"

if ! command -v pio >/dev/null 2>&1; then
  export PATH="${HOME}/.local/bin:${PATH}"
fi

pio run

BUILD="$FW/.pio/build/waveshare43b"
mkdir -p "$OUT"

# pioarduino installs esptool 5 into the PlatformIO venv. esptool 4 uses
# merge_bin / underscores; esptool 5 uses merge-bin / dashes.
ESPTOOL="${HOME}/.platformio/penv/bin/esptool"
if [[ ! -x "$ESPTOOL" ]]; then
  ESPTOOL="$(command -v esptool || true)"
fi
if [[ -z "$ESPTOOL" ]]; then
  echo "esptool not found" >&2
  exit 1
fi
if "$ESPTOOL" merge-bin --help >/dev/null 2>&1; then
  "$ESPTOOL" --chip esp32s3 merge-bin \
    -o "$OUT/grokdesk.bin" \
    --flash-mode dio \
    --flash-freq 80m \
    --flash-size 16MB \
    0x0 "$BUILD/bootloader.bin" \
    0x8000 "$BUILD/partitions.bin" \
    0x10000 "$BUILD/firmware.bin"
else
  "$ESPTOOL" --chip esp32s3 merge_bin \
    -o "$OUT/grokdesk.bin" \
    --flash_mode dio \
    --flash_freq 80m \
    --flash_size 16MB \
    0x0 "$BUILD/bootloader.bin" \
    0x8000 "$BUILD/partitions.bin" \
    0x10000 "$BUILD/firmware.bin"
fi

cp "$FW/web/index.html" "$FW/web/manifest.json" "$OUT/"

python3 - "$OUT/grokdesk.bin" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
data = p.read_bytes()
if data[0] != 0xE9:
    raise SystemExit(f"bootloader magic at 0 is 0x{data[0]:02x}, expected 0xE9")
if data[0x8000:0x8002] != b"\xaa\x50":
    raise SystemExit("partition table magic missing at 0x8000")
if data[0x10000] != 0xE9:
    raise SystemExit(f"app magic at 0x10000 is 0x{data[0x10000]:02x}, expected 0xE9")
print(f"merged {p} ({len(data)} bytes) bootloader@0 partitions@0x8000 app@0x10000")
PY
