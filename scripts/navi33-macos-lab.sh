#!/bin/sh
# Native macOS RX 7600 development baseline: no kext loading or hardware writes.
set -eu

if [ "$(uname -s)" != "Darwin" ]; then
  echo "This script must run on macOS." >&2
  exit 1
fi
cd "$(dirname "$0")/.."

echo "== System baseline (read-only) =="
python3 tools/navi33_macos_baseline.py navi33-macos-baseline.json

echo "== Host-only tests =="
make test

echo "== Build toolchain check =="
if ! xcode-select -p >/dev/null 2>&1; then
  echo "Install Xcode Command Line Tools: xcode-select --install" >&2
  exit 1
fi
if [ ! -d MacKernelSDK ]; then
  echo "MacKernelSDK missing; fetching public headers..."
  sh scripts/bootstrap.sh
fi

echo "== Compile only, do not install/load any kext =="
make DEBUG=1

echo "== Verify signed kext (does not load) =="
codesign --verify build/Debug/RedNavi.kext

echo "== Completed. No GPU registers or driver loading attempted. =="
echo "Review navi33-macos-baseline.json before sharing (no raw EDID/VBIOS collected)."
