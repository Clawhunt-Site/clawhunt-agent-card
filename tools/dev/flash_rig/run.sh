#!/usr/bin/env bash
# tools/flash.py, end to end, against an emulated ESP32-C3 that holds a copy of a real card's flash.
#
#   CARD_DUMP=<whole-flash backup of a card, 8 MB>  CARD_IMAGE=<application image to flash>  run.sh <repo> [rom|stub|both]
#
#   rom   everything through the chip's ROM loader (--no-stub): backup, flash, restore, the flash compared with
#         the original after each. About 17 minutes.
#   stub  with esptool's helper, as on a real card: check, then flash. The emulator does not get through a
#         write by the helper (plain esptool does not either), so the flash stops in the write: what this
#         shows is everything before the write, and what a write that stops half way leaves. About 5 minutes.
#
# PYTHON names a python that has esptool 5 (default: python); QEMU names Espressif's qemu-system-riscv32;
# WORK names a folder for the copies and logs (default: a new one under the system's temp folder).
# The dump holds a card's identity and what it stored: WORK is where copies of it are made, so remove WORK
# when you are done. Nothing here touches a real card.
set -u
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd -- "$1" && pwd)"
what="${2:-both}"
: "${CARD_DUMP:?name a whole-flash backup of a card in CARD_DUMP}"
: "${CARD_IMAGE:?name the application image to flash in CARD_IMAGE}"
py="${PYTHON:-python}"
work="${WORK:-$(mktemp -d)}"
port=socket://127.0.0.1:5555
tool="${repo}/tools/flash.py"
export PYTHONIOENCODING=utf-8

# what the tool said, without esptool's own lines and without the nine lines of the partition table
quiet() { grep -v '^    ' "$1" | grep -v -E '^  [a-z_]+ +(app |data) 0x' | cut -c1-220; }

rom() {
    echo "== $(date +%T) flash, through the ROM loader"
    "${py}" "${tool}" flash "${CARD_IMAGE}" --port "${port}" --no-stub --yes --backup-dir "${work}/backups-rom" > "${work}/rom-flash.txt" 2>&1
    echo "flash exit $?"
    quiet "${work}/rom-flash.txt"
    grep -E 'Read 8388608|Verification|Wrote |Hash of data verified' "${work}/rom-flash.txt" | cut -c1-200
    kept="$(ls "${work}"/backups-rom/card-*.bin | tail -1)"
    "${py}" -X utf8 "${here}/compare.py" "${CARD_DUMP}" "${work}/flash.bin" "${kept}" "${CARD_IMAGE}"
    echo "== $(date +%T) restore, through the ROM loader"
    "${py}" "${tool}" restore "${kept}" --port "${port}" --no-stub --yes > "${work}/rom-restore.txt" 2>&1
    echo "restore exit $?"
    quiet "${work}/rom-restore.txt"
    grep -E 'Wrote |Hash of data verified' "${work}/rom-restore.txt" | cut -c1-200
    if cmp -s "${CARD_DUMP}" "${work}/flash.bin"; then
        echo "after restore: the emulated flash is the original again, byte for byte"
    else
        echo "after restore: the emulated flash is NOT the original"
    fi
}

stub() {
    echo "== $(date +%T) check, with the helper"
    "${py}" "${tool}" check --port "${port}" > "${work}/stub-check.txt" 2>&1
    echo "check exit $?"
    quiet "${work}/stub-check.txt"
    echo "== $(date +%T) flash, with the helper (expected to stop in the write)"
    "${py}" "${tool}" flash "${CARD_IMAGE}" --port "${port}" --yes --backup-dir "${work}/backups-stub" > "${work}/stub-flash.txt" 2>&1
    echo "flash exit $?"
    quiet "${work}/stub-flash.txt"
    grep -E 'Read 8388608|Verification|Wrote |Hash of data verified|fatal error' "${work}/stub-flash.txt" | cut -c1-200
    kept="$(ls "${work}"/backups-stub/card-*.bin | tail -1)"
    "${py}" -X utf8 "${here}/compare.py" "${CARD_DUMP}" "${work}/flash.bin" "${kept}"
}

# the second time round: this script was started again by with_qemu.sh, beside a running emulator
case "${what}" in
    _rom) rom; exit 0 ;;
    _stub) stub; exit 0 ;;
esac

mkdir -p "${work}"
export WORK="${work}"
for part in rom stub; do
    if [[ "${what}" == "${part}" || "${what}" == both ]]; then
        cp "${CARD_DUMP}" "${work}/flash.bin"       # the emulator writes to its flash: never the dump itself
        # ${BASH} and not `bash`: on Windows a plain `bash` can be the one of WSL, which does not see these paths
        "${BASH}" "${here}/with_qemu.sh" "${work}/flash.bin" "${BASH}" "${here}/run.sh" "${repo}" "_${part}"
    fi
done
echo "== $(date +%T) done. Copies of the dump, and the logs, are in ${work}: remove it."
