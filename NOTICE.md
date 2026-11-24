# Third-party work and licences

This repository builds on the work listed below. Each part keeps its own licence.

## Included in the repository

| What | Where | Whose | Licence |
| --- | --- | --- | --- |
| The AI Passport firmware (the base before ClawHunt's changes, with its documentation, tests and skill notes) | `firmware/`, from upstream [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport) at commit `1c9e8b1` (2026-11-12) | FoloToy | MIT, full text in `firmware/LICENSE` (Copyright (c) 2026 FoloToy) |
| Fusion Pixel pixel font (12 px proportional, Simplified Chinese) | `firmware/assets/fonts/`, and the firmware font generated from it | TakWolf and contributors | SIL Open Font License 1.1, full text in `firmware/assets/fonts/OFL.txt` |
| The DNS server of ESP-IDF's `captive_portal` example, changed so that stopping it closes its socket | `firmware/components/dns_server/` | Espressif Systems | Unlicense OR CC0-1.0 (stated at the top of its files) |

The AI Passport hardware and the names and marks "FoloToy" and "AI Passport" belong to FoloToy. The MIT licence
covers the firmware code, not the marks.

This is an independent project. It is not affiliated with or endorsed by FoloToy. "FoloToy" and "AI Passport" are
FoloToy's names and marks, used here only to say which hardware the firmware runs on.

## Fetched when building or running (not in the repository)

| What | Used for | Licence |
| --- | --- | --- |
| ESP-IDF 5.5.3 (including cJSON) | Building the firmware | Apache-2.0 (cJSON: MIT) |
| LVGL 9.5.0 | The firmware's user interface | MIT |
| Espressif components: `button`, `esp_codec_dev`, `esp_lvgl_port`, `cmake_utilities` | The firmware; versions pinned in `firmware/dependencies.lock` | Apache-2.0 |
| esptool 5 | Called by `tools/flash.py` as a separate program | GPL-2.0-or-later |

## ClawHunt's own parts

Everything outside `firmware/` that is not third-party work, and ClawHunt's changes inside `firmware/`, are under
the MIT License, Copyright (c) 2026 Clawhunt-Store (Hong Kong) Intelligent Technology Co., Limited. The full text is
in `LICENSE` at the root of the repository.

The name "ClawHunt" and the crab mark (the images under `firmware/tools/assets/`, and the crab drawn on the card's
screen) are ClawHunt's marks. The MIT License covers the code and the documents, not the marks: they may be used
to refer to ClawHunt and to this project, not to present something else as ClawHunt's.
