#!/usr/bin/env bash
# EKG-Installer für Fedora.
#
#   ./install.sh                Laptop-Software installieren, danach Firmware auf den Pico spielen
#   ./install.sh laptop         nur die Laptop-Software
#   ./install.sh pico           nur die Firmware (mitgelieferte Datei)
#   ./install.sh pico --build   Firmware selbst aus dem Quellcode bauen und aufspielen
#   ./install.sh uninstall      Software, udev-Regel und Startmenü-Eintrag entfernen
#                               (Aufnahmen und Einstellungen bleiben erhalten)
#
# Das Skript ist wiederholbar: Ein erneuter Aufruf aktualisiert alles auf den Stand des Repos.

# Deutsche Anführungszeichen „…“ in Meldungen sind beabsichtigt.
# shellcheck disable=SC1111

set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BIN="$HOME/.local/bin"
RULE_SRC="$REPO/host/packaging/70-pico-ekg.rules"
RULE_DST=/etc/udev/rules.d/70-pico-ekg.rules
DESKTOP="$HOME/.local/share/applications/ekg.desktop"
PYTHON_VERSION=3.13
HAVE_DNF=0

say() { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
ok() { printf ' \033[1;32m✓\033[0m %s\n' "$*"; }
warn() { printf ' \033[1;33m!\033[0m %s\n' "$*" >&2; }
die() {
    printf ' \033[1;31m✗\033[0m %s\n' "$*" >&2
    exit 1
}

usage() { sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

check_system() {
    if command -v dnf >/dev/null 2>&1; then
        HAVE_DNF=1
    else
        warn "Kein dnf gefunden – das Skript ist für Fedora geschrieben."
        warn "Pakete werden nicht installiert; fehlt etwas, bitte selbst nachinstallieren."
    fi
    if [[ $EUID -eq 0 ]]; then
        die "Bitte als normaler Benutzer starten (nicht mit sudo). sudo wird bei Bedarf selbst gefragt."
    fi
}

dnf_install() { # installiert nur, was fehlt
    ((HAVE_DNF)) || return 0
    local missing=() p
    for p in "$@"; do
        rpm -q "$p" >/dev/null 2>&1 || missing+=("$p")
    done
    if ((${#missing[@]})); then
        echo "   Pakete: ${missing[*]}"
        sudo dnf install -y "${missing[@]}"
    fi
}

ensure_uv() {
    if command -v uv >/dev/null 2>&1; then
        return
    fi
    export PATH="$BIN:$PATH"
    if command -v uv >/dev/null 2>&1; then
        return
    fi
    mkdir -p "$BIN"
    if ((HAVE_DNF)) && sudo dnf install -y uv >/dev/null 2>&1; then
        ok "uv aus den Fedora-Paketen installiert"
        return
    fi
    # Rückfall 1: uv von PyPI in eine eigene kleine Umgebung (verändert kein System-Python).
    local boot="$HOME/.local/share/ekg/bootstrap"
    echo "   Installiere uv über PyPI nach $boot …"
    if python3 -m venv "$boot" && "$boot/bin/pip" install -q --upgrade uv; then
        ln -sf "$boot/bin/uv" "$BIN/uv"
        ok "uv installiert"
        return
    fi
    # Rückfall 2: offizieller Installer.
    echo "   Versuche den offiziellen uv-Installer …"
    local script
    script=$(curl -LsSf https://astral.sh/uv/install.sh) || die "uv konnte nicht installiert werden."
    env UV_NO_MODIFY_PATH=1 sh -c "$script"
    command -v uv >/dev/null 2>&1 || die "uv konnte nicht installiert werden."
}

install_udev_rule() {
    if [[ -f $RULE_DST ]] && cmp -s "$RULE_SRC" "$RULE_DST"; then
        ok "udev-Regel ist aktuell"
    else
        sudo install -D -m 0644 "$RULE_SRC" "$RULE_DST"
        sudo rm -f /etc/udev/rules.d/99-pico-ekg.rules # ältere Version dieser Regel
        if command -v udevadm >/dev/null 2>&1; then
            sudo udevadm control --reload-rules || warn "udev neu laden fehlgeschlagen"
            sudo udevadm trigger --attr-match=idVendor=2e8a || true
        fi
        ok "udev-Regel installiert (Zugriff ohne Root, ModemManager bleibt fern)"
    fi
    if ! id -nG "$USER" | tr ' ' '\n' | grep -qx dialout; then
        sudo usermod -aG dialout "$USER"
        ok "Benutzer zur Gruppe dialout hinzugefügt (nur für SSH-Sitzungen nötig)"
    fi
}

install_desktop_entry() {
    mkdir -p "$(dirname "$DESKTOP")"
    sed "s|^Exec=ekg|Exec=$BIN/ekg|" "$REPO/host/packaging/ekg.desktop" >"$DESKTOP"
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$(dirname "$DESKTOP")" >/dev/null 2>&1 || true
    fi
    ok "Startmenü-Eintrag „EKG“ angelegt"
}

install_laptop() {
    say "Laptop-Software installieren"
    dnf_install python3 curl xcb-util-cursor
    ensure_uv
    echo "   Installiere ekg (Python $PYTHON_VERSION, eigene Umgebung; beim ersten Mal ~300 MB Download) …"
    uv tool install --force --python "$PYTHON_VERSION" "ekg[bench] @ file://$REPO/host"
    install_udev_rule
    install_desktop_entry
    case ":$PATH:" in
    *":$BIN:"*) ;;
    *)
        warn "$BIN ist nicht im PATH. Einmalig ausführen und Terminal neu öffnen:"
        warn "    uv tool update-shell"
        ;;
    esac
    "$BIN/ekg" --version >/dev/null || die "ekg startet nicht – Ausgabe oben prüfen."
    ok "$("$BIN/ekg" --version) installiert: ekg, ekg-record, ekg-export, ekg-flash"
}

build_firmware() {
    say "Firmware bauen"
    dnf_install cmake ninja-build gcc-c++ git \
        arm-none-eabi-gcc-cs arm-none-eabi-gcc-cs-c++ arm-none-eabi-newlib
    cmake -S "$REPO/firmware" -B "$REPO/build/firmware" -G Ninja
    cmake --build "$REPO/build/firmware"
    ok "gebaut: build/firmware/ekg.uf2"
}

install_pico() {
    local build=0
    [[ ${1:-} == --build ]] && build=1
    if [[ ! -x "$BIN/ekg-flash" ]]; then
        install_laptop # das Flash-Werkzeug gehört zur Laptop-Software
    fi
    local uf2="$REPO/firmware/prebuilt/ekg.uf2"
    if ((!build)) && ! "$REPO/tools/update_prebuilt.sh" --check >/dev/null 2>&1; then
        warn "Die mitgelieferte Firmware passt nicht zum Quellcode – sie wird neu gebaut."
        build=1
    fi
    if ((build)); then
        build_firmware
        uf2="$REPO/build/firmware/ekg.uf2"
    fi
    say "Firmware auf den Pico spielen"
    echo "   Pico per USB anschließen. Läuft schon die EKG-Firmware, geht der Rest automatisch."
    "$BIN/ekg-flash" "$uf2"
}

uninstall() {
    say "Deinstallieren"
    if command -v uv >/dev/null 2>&1 || [[ -x "$BIN/uv" ]]; then
        PATH="$BIN:$PATH" uv tool uninstall ekg || true
    fi
    rm -f "$DESKTOP"
    if [[ -f $RULE_DST ]]; then
        sudo rm -f "$RULE_DST"
        if command -v udevadm >/dev/null 2>&1; then
            sudo udevadm control --reload-rules || true
        fi
    fi
    ok "entfernt. Aufnahmen: ~/.local/share/ekg, Einstellungen: ~/.config/ekg (bleiben erhalten)"
}

finish() {
    say "Fertig"
    cat <<EOF
   Starten:            ekg              (oder im Startmenü „EKG“)
   Ohne Hardware:      ekg --simulate
   Firmware erneut:    ./install.sh pico

   Vor der ersten Messung: Laptop-Ladekabel ziehen, nichts mit Netzanschluss am Laptop.
   Kein Medizinprodukt – keine Diagnose.
EOF
}

main() {
    case "${1:-}" in
    -h | --help | help)
        usage
        return
        ;;
    esac
    check_system
    case "${1:-all}" in
    all)
        install_laptop
        echo
        read -rp "Jetzt die Firmware auf den Pico spielen? [J/n] " answer
        if [[ ! ${answer:-j} =~ ^[nN] ]]; then
            install_pico
        fi
        finish
        ;;
    laptop)
        install_laptop
        finish
        ;;
    pico)
        shift
        install_pico "$@"
        ;;
    uninstall) uninstall ;;
    *)
        usage
        exit 2
        ;;
    esac
}

main "$@"
