# Checking `tools/flash.py` without a card

Two checks, neither of which touches a real card. Run both after changing `tools/flash.py` and before releasing a
new version.

## 1. Do the tests catch a broken tool?

```bash
python tools/dev/flash_rig/mutate.py .
```

On a copy, it takes the tool's safeguards out one at a time (writing from `0x0`, not checking the partition table,
not backing up, not asking, not having the chip confirm the backup, not looking at the card again before the
write, and so on; 35 in all), and runs the tests after each; a test must go red. `NOT CAUGHT` means the tests have
a hole; `NOT TRIED` means the tool was edited and the list has to be updated with it. About 40 seconds per change,
twenty-odd minutes in all.

## 2. The whole procedure in the emulator, on a real card's flash

You need three things:

- Espressif's QEMU (the one that knows the `esp32c3` machine):
  `python $IDF_PATH/tools/idf_tools.py install qemu-riscv32`.
- A Python with esptool 5 (`pip install -r tools/requirements.txt`).
- A whole-flash backup of a card (8 MB) and the application image to flash. **A whole-flash backup holds that
  card's identity and what it stored, and does not go into the repository**; the script only reads it and gives the
  emulator a copy to write to.

```bash
CARD_DUMP=<whole-flash backup.bin> CARD_IMAGE=<application image.bin> QEMU=<path to qemu-system-riscv32> PYTHON=<python> \
    bash tools/dev/flash_rig/run.sh . both
```

On Windows, run it in Git Bash. When starting it from another program, give the full path of Git Bash's `bash`: the
`bash` that comes with the system is WSL's, and it does not see these drive-letter paths.

- `rom` (about 17 minutes): the chip's own loader all the way (`--no-stub`). Backup, flash, restore; after each step
  the emulator's flash is compared with the original byte for byte. Expect: the backup equals the original; before
  and after the application partition nothing changed; the application partition starts with the image; after
  restore the whole flash equals the original.
- `stub` (about 5 minutes): with esptool's helper, as on a real card. `check`, the whole-flash backup and the chip's
  confirmation of the backup all complete; in the emulator the write stops at about 128 KB, when the chip no longer
  answers (the same happens with plain esptool, without this tool), so the flash in this run is **expected to
  fail**. What to look at: every step before the failure is right, the tool says the write did not finish and how
  to go back, and all changed bytes are inside the application partition.

When it is done, delete the `WORK` folder (the script says where it is at the end): it holds copies of the
whole-flash backup.

The results of 2026-11-24 are at the end of `docs/REPLICATE.md`.

## The emulator is not a real card

It does not have the real card's USB port (entering download mode by itself and restarting after flashing cannot
be checked), its MAC is all zeros, and writes through the helper do not get through. What it shows is that every
command this tool sends to esptool does to a real factory flash what the tool says it does.
