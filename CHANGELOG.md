# Changelog

This file records the repository and the images it releases. What changed in each firmware version is in the
version table at the end of `firmware/docs/clawhunt-agent-card.md`.

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
