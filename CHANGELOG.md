# Changelog

This file records the repository and the images it releases. What changed in each firmware version is in the
version table at the end of `firmware/docs/clawhunt-agent-card.md`.

## v0.11.0 (2026-11-25): a new face

The firmware changed; the flashing tool did not. `clawhunt-agent-card-0.11.0-reference.bin` in the release is the
image that runs on the reference card (SHA-256 `665e8b2800bacf052e7d5bcd4de291dcf77265ac176fae90c82724cdfd2de95c`).

- **A new look.** One dark ground and one accent colour, mint, kept for the one number of each screen, today's bar
  and the live state. Amber says only "cannot pay now" and "did not go through"; red says only that the card is
  frozen. Everything is set from the left edge, and each sheet names itself in a tag at the top. Latin and digits
  are in Cascadia Mono (SIL OFL 1.1), smoothed; Chinese stays in the Fusion Pixel font.
- **The face is built around one number:** what the agent has earned in total, with the exact amount under it, up
  to three facts, and a week of daily earnings as bars. One word in the header says what the agent is doing. Until
  the card has been told its agent's numbers, the number is `--`, where 0.10 showed `$0.00`.
- **The card keeps the week itself,** from the total it is told and the `Date` header of the server's answers. It
  shows what it saw: a day without the card has no bar, and a card that has just been flashed starts with an empty
  week. See "The week of earnings" in the firmware guide.
- USB `stats` takes two more optional fields, `week` and `wd`. The server API, what the keys do and the words on
  the sheets are unchanged.
- The application is smaller, 2,428,192 bytes against 2,472,464 for 0.10.0: the gradient grounds and three large
  pixel fonts are gone.

Checked on a computer on 2026-11-25:

- The build (ESP-IDF 5.5.3, from an empty build folder) has no compiler warning, and `tools/flash.py plan
  --expect-version 0.11.0` takes the image.
- The firmware's host tests pass, with new tests for the week: a total that falls and comes back, answers dated
  oddly, a restart, another server.
- Every screen was drawn on a computer by the firmware's own screen code, three times: with an agent's public
  numbers, with a new card's, and with the longest and largest values. Nothing runs into anything.
- What the screens hold on the heap was counted the same way for 0.10 and 0.11: the face holds about 3.6 kB
  more and the most held at once is about 4.3 kB more (sizes of a 64-bit computer, an upper bound for the card);
  nothing is left behind after a walk through all screens. At start the heap is 224 bytes smaller.
- Two independent reviews read the code, one for what comes in from outside and one for the screen code. Neither
  found a way to crash the card, corrupt its memory or leak its token; what they did find was fixed (the week
  could add earnings it had not seen; a job's progress was said again at the foot with every step).

On the reference card, the same day:

- `tools/flash.py flash` wrote the image: the first time the tool wrote to a real card. Backup, write and the
  chip's confirmation of both went through; the card came back paired, with its Wi-Fi and its identity. The whole
  flash, read again afterwards, differs from the backup only in the application partition and in what the
  firmware itself stored in `nvs` (the record is at the end of `docs/REPLICATE.md`).
- The card runs 0.11.0 paired with the site and reads its agent with its token. Free heap at rest is 69 kB and
  stayed level over ten minutes (0.10: 72 kB); its lowest in that time was 61 kB (0.10: 65 kB).
- Every screen was walked through over USB, with a picture of each: 27 checks. On the first walk two of them
  failed, both through faults of the walk's own script (it looked at a sheet that stands for 2.4 seconds after
  3 seconds, and it took the quiet tag of the "not paid" sheet for the ground); looked at again with the clock,
  both sheets stand as long as they should, and with the script corrected all 27 pass.

Not checked: the look of the physical screen (0.11.0 is the first version with smoothed type), the keys and the
sound with this version; a bar of the week growing (the reference card's agent has not earned since).

## 2026-11-25: public edition

The firmware and the flashing tool did not change, and there is no new version number.

- The documents are in English: `README.md`, `docs/READINESS.md`, `docs/REPLICATE.md`, `NOTICE.md`, this file, and
  a firmware guide written for people outside the project, `firmware/docs/clawhunt-agent-card.md`.
- ClawHunt's own parts are under the MIT License (`LICENSE`, `NOTICE.md`).
- The firmware's host tests run from a copy of the repository without its history: the 0.7.6 `card_app.c` they
  compare against is kept as `firmware/tests/card_app_old.c`.
- Comments were tidied (the generated fonts' headers, one line in `firmware/main/cloud.c`), the host test's stand-in
  server uses a made-up private address, and the firmware's two helper scripts that read the site's public
  interface name themselves in their User-Agent. No compiled line changed.
- The public edition is published as one commit, without the development history.

## 2026-11-25: `check` ran on the reference card

`python tools/flash.py check` ran once on the reference card: it recognised the ESP32-C3, the 8 MB flash, the
factory partition table and the firmware flashed on 2026-11-18 (version string `24ab315-dirty`), and wrote
nothing; the card came back online as usual after the restart. Recorded at the end of `docs/REPLICATE.md`. `flash` and `restore` have still not run on a real card.
Neither the image nor the tool changed; there is no new version number.

## v0.10.0 (2026-11-24): standard edition

The firmware did not change: it is still the 0.10.0 flashed onto the reference card on 2026-11-18. This version
arranges the repository so that following it is enough to replicate a card.

- **One command to build:** `build.sh` (in Espressif's ESP-IDF 5.5.3 container pinned by digest in Docker, or with a
  local ESP-IDF) and `build.ps1` (with a local ESP-IDF on Windows). The image lands in `dist/`, named after the
  firmware version, and the version is written into the image as well. The images from both ways of building differ
  from the one on the reference card only in build stamps.
- **One command to flash:** `tools/flash.py`, with four commands: `plan`, `check`, `flash`, `restore`. It writes only
  the application partition; it checks the image first, and the card's partition table must match the factory
  layout entry for entry; it backs up the whole flash first and has the chip confirm the backup; what it writes are
  the bytes that were checked. It comes with 76 tests that need no card; backup, flash and restore were run in the
  ESP32-C3 emulator on a copy of a factory flash; it went through two rounds of independent review (security,
  correctness). How to repeat these checks without a real card: `tools/dev/flash_rig/`.
- **No automatic build:** there is no CI for this repository yet. Builds and tests run on a desk, and the release
  images are built there and uploaded.
- **Documents:** `docs/READINESS.md` (what is missing before real use), `docs/REPLICATE.md` (replicating a card),
  `NOTICE.md` (third-party work and licences).
- The scripts used during development are kept apart from the flashing tool, under `tools/dev/`.
- `clawhunt-agent-card-0.10.0-reference.bin` in the release is the image that ran on the reference card
  (SHA-256 `dcd90daed1bc2b22ec3116d7bdb28e5cada58393868c660b003e6f21b4a26704`).

Not done: `flash` and `restore` have not run on a real card; the agent identity built into the firmware has not
been removed.

## Before

Firmware 0.1 to 0.10 is summarised, version by version, in the firmware guide.
