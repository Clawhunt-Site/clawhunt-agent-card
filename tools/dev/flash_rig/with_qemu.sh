#!/usr/bin/env bash
# Run a command against an emulated ESP32-C3 (Espressif's QEMU) that sits in download mode, as a card does
# while it is flashed. Its flash is the file named first; its serial port is tcp 127.0.0.1:5555, which esptool
# reaches as --port socket://127.0.0.1:5555. The emulator is started here and stopped here.
#
#   with_qemu.sh <flash file> <command ...>
#
# QEMU names the emulator (default: qemu-system-riscv32 on the PATH). Espressif's build is the one that knows
# the esp32c3 machine: `python $IDF_PATH/tools/idf_tools.py install qemu-riscv32` fetches it.
# The flash file is WRITTEN by what the command does: give it a copy, never a backup you want to keep.
set -u
flash="$1"
shift
qemu="${QEMU:-qemu-system-riscv32}"
"${qemu}" -display none -monitor none -machine esp32c3 -drive "file=${flash},if=mtd,format=raw" \
    -global driver=esp32c3.gpio,property=strap_mode,value=0x02 -serial tcp:127.0.0.1:5555,server,nowait > "${flash}.qemu.log" 2>&1 &
pid=$!
trap 'kill "${pid}" 2>/dev/null; wait "${pid}" 2>/dev/null' EXIT
sleep 3
if ! kill -0 "${pid}" 2>/dev/null; then
    echo "the emulator did not start: $(cat "${flash}.qemu.log")" >&2
    exit 1
fi
"$@"
code=$?
echo "command exit ${code}"
exit "${code}"
