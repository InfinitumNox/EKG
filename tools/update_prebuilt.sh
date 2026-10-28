#!/usr/bin/env bash
# Baut die Firmware und legt sie als firmware/prebuilt/ekg.uf2 ab.
#   tools/update_prebuilt.sh          bauen und aktualisieren
#   tools/update_prebuilt.sh --check  nur prüfen, ob die mitgelieferte Datei zum Quellcode passt
set -euo pipefail
cd "$(dirname "$0")/.."
hash=$(tools/firmware_hash.sh)
if [[ "${1:-}" == "--check" ]]; then
    stored=$(cat firmware/prebuilt/SOURCE_HASH 2>/dev/null || true)
    if [[ "$hash" != "$stored" ]]; then
        echo "firmware/prebuilt/ekg.uf2 ist veraltet. Bitte tools/update_prebuilt.sh ausführen." >&2
        exit 1
    fi
    echo "prebuilt passt zum Quellcode ($hash)"
    exit 0
fi
cmake -S firmware -B build/firmware -G Ninja
cmake --build build/firmware
cp build/firmware/ekg.uf2 firmware/prebuilt/ekg.uf2
echo "$hash" > firmware/prebuilt/SOURCE_HASH
echo "firmware/prebuilt/ekg.uf2 aktualisiert ($hash)"
