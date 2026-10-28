#!/usr/bin/env bash
# Prüfsumme über alle Firmware-Quellen. Damit erkennen install.sh und die CI, ob
# firmware/prebuilt/ekg.uf2 zum aktuellen Quellcode passt.
set -euo pipefail
cd "$(dirname "$0")/.."
find firmware/CMakeLists.txt firmware/pico_sdk_import.cmake firmware/src -type f -print0 \
    | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1
