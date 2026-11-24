# Replicate a card

How to turn a retail FoloToy AI Passport into a ClawHunt Agent Card that is the same as the reference card. On the
computer it takes two commands (get the image, flash it); on the card and the phone, four steps (join Wi-Fi, pair,
set the secret, write the NFC tag).

**This card moves no money.** Confirming a payment only records one row on the server. What is missing before it
can really take and make payments: [READINESS.md](READINESS.md).

## What you need

- A FoloToy AI Passport (ESP32-C3, 8 MB flash). The flashing tool checks its partition table first and stops if it
  does not match.
- A USB-C cable that carries data.
- A computer (Windows, macOS or Linux) with Python 3.9 or later.
- A ClawHunt account with an agent that passed review and is publicly visible. Only such agents can be paired with
  a card.
- 2.4 GHz Wi-Fi, or a phone hotspot (on an iPhone, turn on "Maximize Compatibility"). The card does not support
  5 GHz.
- If others should be able to open the card's page with a tap: a phone that can write NFC tags.

## 1. Get the image

Two ways; pick one.

**Download.** Release `v0.10.0` holds two images:

| File | What it is |
| --- | --- |
| `clawhunt-agent-card-0.10.0-reference.bin` | The image flashed onto the reference card on 2026-11-18. SHA-256 `dcd90daed1bc2b22ec3116d7bdb28e5cada58393868c660b003e6f21b4a26704`. The version string inside it is `24ab315-dirty`, not `0.10.0`: it was built before the version number was passed to the build |
| `clawhunt-agent-card-0.10.0-app.bin` | Built from the same source on 2026-11-24 with `build.sh --docker`. Never on a card; differs from the one above only in build stamps (86 bytes). Its version string is `0.10.0` |

```bash
gh release download v0.10.0 -R Clawhunt-Site/clawhunt-agent-card -p "*-reference.bin"
```

**Build it yourself.** With Docker (on Windows, run it in Git Bash):

```bash
./build.sh --docker
```

Or in a shell where ESP-IDF 5.5.3 is set up (an `idf.py` of another version stops the build):

```bash
./build.sh
```

On Windows without Docker, with a local ESP-IDF:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -IdfPath C:\esp\esp-idf-v5.5.3 -IdfToolsPath C:\esp\tools
```

The image lands in `dist/clawhunt-agent-card-0.10.0-app.bin`. To install ESP-IDF, follow Espressif's ESP-IDF 5.5.3
Get Started guide.

The firmware has the id and name of one publicly listed ClawHunt agent built in (not the agent the reference card
is paired with), and the card shows that agent's public profile until it is paired. To have it show your agent from
the first start, run this before building:

```bash
python firmware/tools/bind_identity.py --agent <your agent id>
```

You can also skip it: after pairing, the card shows the agent you paired it with.

## 2. Flash it

```bash
pip install -r tools/requirements.txt
python tools/flash.py check
python tools/flash.py flash clawhunt-agent-card-0.10.0-reference.bin
```

`check` only reads: whether the chip is an ESP32-C3, whether the partition table is the AI Passport's factory
layout, and which firmware is on the card now.

`flash` does the following in order and stops at the first step that does not hold. When it stops before the
write, nothing on the card has changed, and a card that had answered is restarted so that it does not stay waiting
in its loader:

1. Check the image: whether it is a whole, undamaged ESP32-C3 application (its checksum and its own SHA-256 are
   recomputed), and whether it is this card's project. The file is read only this once: what is written to the card
   are the bytes that were checked, even if another program changes the file in the meantime.
2. Check the card: the chip is an ESP32-C3 with 8 MB of flash; the partition table is the same as the AI Passport's
   factory table, entry for entry. If a single entry differs, nothing is flashed: somebody has to look at that card
   first.
3. Read the whole flash (8 MB) into `clawhunt-card-backups/` in your home folder, which takes a minute or two; then
   have the chip compute a hash of its own flash and compare it with the file. A bad read is not kept as a backup.
4. Ask you once; you have to type `yes`.
5. Look at the card once more: it continues only if it is still the same card (MAC and partition table unchanged).
6. Write the image to `0x10000`; the chip computes a hash of what was written and it is compared with the image's.
7. Restart the card.

Only the application partition is written. The bootloader, the partition table, the card's own identity
(`cardid`), its saved Wi-Fi networks and its pairing with ClawHunt are left alone. **Do not** flash a merged image
from `0x0` as the upstream documentation describes: that erases `cardid`. This tool has no command that writes
anywhere else.

The backup file is named `card-<MAC>-<time>.bin`. It is everything on the card, including its identity, its saved
Wi-Fi passwords and its device token; if the card's identity is lost, only this file can bring it back. Keep it
safe, do not give it to anyone, and do not put it in any repository. To keep it elsewhere, use `--backup-dir`.
`--no-backup` skips step 3; use it only on a card whose whole-flash backup you already have.

If the write is interrupted (a loose cable, the computer went to sleep): the application on the card is incomplete
and the card may not start, but nothing outside the application partition was touched. Reconnect and run the same
command again, or go back with `restore` below.

If the card is not found: try another data cable; press a key on the card to wake it; if the computer has more than
one Espressif serial port, name it with `--port COM6`. If esptool's helper does not run, add `--no-stub`: much
slower, but it works.

## 3. Join Wi-Fi

1. **Hold OK** (about half a second) to open the settings, choose `连 Wi-Fi` ("join Wi-Fi"; the card's screens are
   in Chinese) with the up and down keys, confirm with OK.
2. The card opens its own hotspot `ClawCard-XXXX` and shows a QR code on the screen.
3. Scan the code with the phone camera to join the hotspot; the setup page opens by itself. If it does not, open
   `192.168.4.1` in a browser.
4. Choose the network and enter the password. The card's screen and the page both show "connecting / connected /
   not connected".
5. Once connected, the card remembers the network (up to 4) and closes its hotspot after a few seconds.

## 4. Pair

Once the card is online, it shows an 8-character pairing code, in two lines of four.

1. Open `https://clawhunt.store/card/pair` on a computer or phone and sign in.
2. Enter the pairing code and choose one of your agents.
3. The page asks you to compare the code shown on the card; tick the box, then confirm.
4. The card receives its device token, and its face changes to this agent.

## 5. Set the secret

Still at `/card/pair`, set a secret for this card: 8 presses of the up, down and OK keys, in any order. Nobody can
start a payment request to a card that has no secret.

The example secret in the firmware guide and the tests is nobody's real secret; do not use it. Secrets that are too
simple (for example one key pressed 8 times) are refused by the site.

After 5 wrong tries in a row the card freezes. When the card is not with you, you can freeze it on this page first,
without unpairing it.

## 6. Write the NFC tag (optional)

The back of the card carries an NTAG213 tag. It is not connected to the card's chip; what it holds has to be
written with a phone.

1. On `/card/pair`, a paired card has a link of the form `https://clawhunt.store/c/<the card's public id>`.
2. With an app on the phone that can write NFC tags, write a URL record with this address to the tag on the back of
   the card.

The tag's content can be copied by others, so a tap only opens the page; every payment has to be confirmed on the
card.

## Try a payment

Tap the card with another phone, or open that address directly, enter an amount and start a request. The card
rings and shows who is paid, how much, and a three-digit check code (the same as on the other person's page). Hold
OK, then press the 8 keys of the secret. The other page shows "confirmed". The server recorded one row; no money
moved.

## Back to the factory firmware

```bash
python tools/flash.py restore ~/clawhunt-card-backups/card-<MAC>-<time>.bin
```

It cuts the application out of the whole-flash backup and writes it back to `0x10000`, so the factory application
is back. It checks first as well: that the backup is a whole-flash backup of an AI Passport, that the application
in it is whole, and that the card's partition table is right; then it asks once.

`restore` does not remove what this firmware stored on the card: the Wi-Fi networks and passwords you saved and
the device token stay in the `nvs` partition, under the firmware's own namespace `clawcard`, in plain text; the
factory application does not use them. To have them gone, do this before you restore: unpair the card at
`/card/pair` (the card drops its token the next time it asks the server) and forget the saved networks on the
card's Wi-Fi setup page.

## Do not

- Do not flash anything from `0x0`.
- Once the card is paired, do not send it `unpair` over USB: that really unpairs it, pairing again gives it a new
  public id, and the NFC tag has to be written again.
- Do not put backup files or Wi-Fi passwords into any repository or chat.
- Example numbers are examples. Do not present demonstration data as real results.

## How far this procedure was verified (2026-11-24; the real-card item on 11-25)

**On the real card: only `check` has run.** On 2026-11-25, `tools/flash.py check` ran once on the reference card
(the tool as of tag `v0.10.0`, esptool 5.6.0, with the helper, 3 seconds, exit code 0):

- It found the port by itself (the only Espressif USB port on that computer); chip ESP32-C3 (QFN32, rev v1.1),
  8 MB flash, the MAC of the reference card.
- The partition table is the same as the factory layout, entry for entry, nine partitions; the tool said the
  firmware can be written.
- The firmware on the card now: project `FoloToy-AI-Passport`, version string `24ab315-dirty`, built 2026-11-18
  12:41:06, which is the image flashed on 11-18. (That string is git's description of the development tree the
  image was built from, before the version number was passed to the build; the builds from this repository's
  source, which differ from that image in the build stamp only, show that the code is the same.)
- Nothing was written. At the end the tool restarted the card; 16 seconds later the server's public interface showed
  the card online, and three more looks within the following minute all showed it online and still able to take
  payment requests, meaning it came back with its Wi-Fi and device token.

`flash` and `restore` have not yet run on any real card. Steps 3 to 6 (Wi-Fi, pairing, secret, NFC) are the steps
the reference card actually went through; what the card does at each of them is described in the firmware guide
(`firmware/docs/clawhunt-agent-card.md`). What follows is what can be shown without a real card; how to run the
emulator and the "break the tool" checks again is in `tools/dev/flash_rig/README.md`.

**Tests** (`python -m unittest discover -s tools/tests -v`, 76 of them, no card needed; 2 of them need real card
backup files and run only on a machine that holds those backups, 1 checks file permissions and runs only on Linux
and macOS). The partition table used is the one ESP-IDF builds from `firmware/partitions.csv`, which is identical
byte for byte to the partition table in the reference card's factory flash. The "card" is an 8 MB file in which
every byte outside the partition table is filled with non-empty data, compared byte for byte after flashing: every
byte before the application partition, and after the last 4 KB sector holding the image, is unchanged. Things going
wrong are covered too: the image file changed while the tool works, extra bytes after the image, a backup that
reads back with one byte wrong or one byte short, a different card on the port after the question, esptool dying
half way through the write, a chip that connects and then stops answering, a partition table with one entry more or
one less, or an application partition that overlaps `cardid`. The tool was deliberately broken in 35 places
(writing from `0x0`, not checking the partition table, not backing up, not asking, not having the chip confirm the
backup, not looking at the card again before the write, reading the file again before writing, writing the whole
backup back on restore, not restarting the card after an error, and so on); for each, a test goes red.

**Independent review**, two rounds, both by reviewers who did not take part in writing the tool. The first round
asked "is there any way to make it write outside the application partition, or write something that was not
checked": no critical issue, one high-risk issue (it checked the file, then read the file again for the write),
fixed; the other tightenings it suggested were all made too (the partition table must match the factory layout
entry for entry, the backup must be confirmed by the chip, the card is looked at again before the write, the paths
given to esptool cannot be taken as an argument file). The second round asked "does it do what it says, and does it
run on other systems": no high-risk issue; what it pointed out was fixed as well (a chip that fails after it
connected was left in its loader, `flash` also wrote the extra bytes after the image, the backup was readable by
others while it was being read, and on Windows the line endings of `build.sh` were broken in a freshly cloned
repository).

**Emulator.** Espressif's QEMU (9.2.2, the `esp32c3` machine) with esptool 5.6.0; the flash is a copy of the
reference card's whole factory backup, the chip is in download mode, and esptool drives it over the serial line as
when flashing a real card:

| Step | Result |
| --- | --- |
| `check` | Recognised ESP32-C3, 8 MB, nine partitions; the card holds the factory application `trae_card` 1.2.2 |
| Backup in `flash` | The 8,388,608 bytes read are identical to the original flash byte for byte (the same SHA-256 as the reference card's factory backup); the hash computed by the chip matches the file |
| Write in `flash` | After looking at the card again, 2,472,464 bytes were written to `0x10000`, and esptool reported matching hashes. Whole flash compared afterwards: before the application partition (bootloader, partition table, `nvs`, `phy_init`) and after it (images, `cardid`, audio), unchanged byte for byte; the application partition starts with this image; 1,517 bytes after the image changed, the tail of the erased factory application, all inside the application partition |
| `restore` | The backup's application (3,145,728 bytes) written back to `0x10000`, hashes match. The whole flash is identical to the original byte for byte |
| Interrupted write | The tool said the write did not finish and how to go back. Whole flash compared: all 180,946 changed bytes are inside the application partition, everything before and after it unchanged byte for byte; the backup taken before flashing is identical to the original |

Where the emulator differs from a real card:

- esptool normally uploads a helper to the chip before it works. The helper's **reads** work in the emulator
  (`check`, the whole-flash backup and the chip's confirmation of the backup all ran this way); its **writes** stop
  at about 128 KB in the emulator, when the chip no longer answers, and the same happens with plain esptool, without
  this tool (that is where the "interrupted write" row comes from). So the complete write and restore in the table
  used `--no-stub`, the chip's own loader. Every earlier flash of the real card used the helper and never had this
  problem.
- The rows that used the chip's own loader were run again in full after the changes from the second review, with
  the same results; since then the tool changed in one place only: a backup file whose name is taken gets another
  name. The "`check`" and "interrupted write" rows were run with the helper, before the changes from the second
  review; those changes are not on the path these two rows take, except the decision to restart the card after an
  error, which has a test of its own.
- The emulator has no real card USB port (on a real card, it puts the card into download mode and restarts it when
  done), and its MAC is all zeros. Finding the serial port by itself and restarting the card after reading were
  verified with a fake esptool in the tests, and the real-card `check` above went through them too; the restart
  after a write has been verified only in the tests.

**Building.** Both ways were actually run, and each image differs from the one on the reference card only in build
stamps (the version in the application description, the time, the ELF hash, and the checksum at the end):
`build.sh --docker` in the ESP-IDF 5.5.3 container pinned by digest (from a clean export, 86 bytes different), and
`build.ps1` natively on Windows (84 bytes different). The two images differ from each other by 70 bytes, all in the
same places. `build.sh` without Docker, with a local ESP-IDF, and macOS: nobody has run them.

**Other Pythons and systems.** The tests pass on Windows with Python 3.12 and 3.9, and on Linux (WSL Ubuntu) with
Python 3.12. Nobody has run them on macOS.
