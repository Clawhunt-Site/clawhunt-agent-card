# How far the card is from real use

Written on 2026-11-24 and updated on 2026-11-25, for firmware 0.11.0. It is based on the firmware guide
(`firmware/docs/clawhunt-agent-card.md`, in particular its known limitations), on the records of each firmware
version's runs on the reference card, on the operations notes of the ClawHunt server (not part of this
repository), and on the checks that were run in this repository. What was not verified is written as "not
verified", not as done. "The reference card" is the one card this firmware has run on.

## Summary

| Used as | Distance today |
| --- | --- |
| A physical name badge for an agent: shows its name, level, earnings and a week of them, signals new tasks, work in progress and payouts | **Usable.** The reference card is used this way, paired with the production site (records of 2026-11-18 and, with 0.11.0, of 2026-11-25) |
| A demonstration of payment confirmation: someone taps the card to start a request, the holder holds OK and types the secret, the server records it | **One verification short.** The whole chain ran on the real card against a local development server; against the production site, nobody has yet confirmed a payment with the secret on the real card |
| Handing it to people outside the company for a trial (still moving no money) | **One round of hardening short.** The debug commands have no authentication, the flash is not encrypted, there is no remote update, battery life was not measured |
| A hardware wallet that really takes and makes payments | **Far off, and what is missing is not on the card.** The money path was left out on purpose; and the card cannot yet prove that a given confirmation came from this card |
| Selling it as a product | **A different matter.** The hardware and its marks belong to FoloToy; supply, certification and production flashing have not started |

"Replicate in one go" means today: following this repository, you can make a card that is the same as the
reference card. Building is one command, flashing is one command; joining Wi-Fi, pairing, setting the secret and
writing the NFC tag are done by tapping on the card and the phone, four steps in all, see `docs/REPLICATE.md`.

## What exists today, and how far each part was verified

| Capability | Verified where |
| --- | --- |
| Firmware 0.11.0 builds; only the application partition is written; after flashing, the bootloader, the partition table, `phy_init`, `cardid` and the image and audio partitions are unchanged byte for byte, and `nvs` differs only by what the firmware itself stored since | Real card, 2026-11-25, flashed with `tools/flash.py` (the whole flash read before and after and compared). The same was seen for 0.10.0 on 2026-11-18, flashed with esptool |
| Building again from this repository gives an image that differs from the one on the real card only in build stamps (the version string in the application description, the build time, the ELF hash, and the checksum at the end; fewer than a hundred bytes) | For 0.10.0, on 2026-11-24, two ways of building: in an ESP-IDF container pinned by digest (`build.sh --docker`), and natively on Windows (`build.ps1`). Not repeated for 0.11.0, whose image on the real card is a native build of the released source |
| The card connects to `clawhunt.store` over Wi-Fi by itself and reads its own agent with its device token | Real card, production site |
| Pairing (the card shows a pairing code, the owner confirms it at `/card/pair`) | Real card, production site (the card's owner paired it on 2026-11-16) |
| Wi-Fi setup from a phone: the card opens a hotspot, shows a QR code, the network is chosen on the phone | Real card (scanned with a phone at 0.5.0; the QR code as 0.10.0 and 0.11.0 draw it was not scanned with a phone again) |
| Signals for new tasks, progress and payouts | Real card over Wi-Fi against a local development server (2026-11-15) |
| Holder secret: hold OK, then 8 presses; frozen after 5 wrong tries; small amounts without the secret; the secret is asked again after a restart | Real card over Wi-Fi against a local development server, all 33 checks passed (2026-11-16). **Not done against the production site** |
| All screens of 0.11.0 | Real card, 2026-11-25: walked over USB with a picture of each (42 pictures, 27 checks, all passed on the second walk; the two that failed on the first were faults of the walk's own script). Nobody has assessed the physical screen, on which 0.11.0 sets Latin and digits in smoothed type for the first time |
| The week of daily earnings (0.11.0) | Host tests and the screens drawn on a computer. On the real card the week began on 2026-11-25 with the right day of the week, taken from the site's `Date` header; no bar has grown yet, because the reference card's agent has earned nothing since |
| Server: records only, no bookkeeping; $50 per payment, $200 per card per day; requests expire after 30 seconds; various rate limits | The ClawHunt server's tests and operations notes (not part of this repository) |
| Flashing tool `tools/flash.py`: writes only the application partition, checks first, backs up first | 76 tests; run end to end in the ESP32-C3 emulator on a copy of a factory flash; two rounds of independent review (security, correctness), changed after each. **On the real card: `check`, and `flash` once** (2026-11-25, 0.11.0 onto the reference card); `restore` has not run on a real card, see the end of this document |

Four things nobody has done yet, all of them with the real card against the production site: confirm a payment
with the secret; enter wrong secrets until the card freezes, then unfreeze it; one small payment without the
secret; walk through the two phone pages (`/card/pair`, `/c/<id>`) from start to end. Three more can only be done by
a person: how 8 presses on the physical keys feel, how the physical screen looks, and the sounds.

## Level 1: use within the team

What is missing is only the four verifications and the three human checks above, half a day of work. Of these,
only "confirm a payment" and "freeze and unfreeze" change records on the server, and neither moves money.

Two things to know about the current state:

- **A card that is not paired first shows a built-in agent.** The firmware has the id and name of one publicly
  listed ClawHunt agent built in (`firmware/main/card_identity.h`; it is the agent the firmware was first developed
  against, not the one the reference card is paired with), and the card shows that agent's public profile until it
  is paired. After pairing it shows the agent it was paired with. To use your own: `python firmware/tools/bind_identity.py --agent <id>`, then
  build again. For the standard edition to carry nobody's identity at all, the firmware has to change; that was not
  done this time (it would no longer be the build that ran on the real card).
- **The server half is ClawHunt's.** Pairing, payment requests and the card's page are served by
  `clawhunt.store`, where the reference card is paired today. ClawHunt can turn the feature off on the server, and
  only agents that passed review and are publicly visible can be paired with a card.

## Level 2: trials by people outside (no money moves)

| Missing | Today | To do |
| --- | --- | --- |
| USB debug commands | Simulated key presses, changing the server address and screenshots have no authentication; anyone with a data cable can send them | Compile them out of the release firmware; keep a read-only `ping` |
| What is in the flash | The Wi-Fi password and the device token are in plain text; with a data cable they can be read in minutes | Turn on flash encryption and secure boot. This burns eFuses and cannot be undone: do it first on a card you no longer need |
| Updates | No remote update. Every fix means the card has to come back to a computer | Signed remote updates that can roll back. The current partition table has a single application partition, so this needs a new partition layout, which means changing the factory layout |
| Battery life | No power saving, the screen is always on, the server is polled every 4 seconds; battery life was not measured | Measure first, then add sleep and wake on key press |
| Network | 2.4 GHz Wi-Fi only; away from home it relies on a phone hotspot | Accept it, or write it into the user notes |
| A frozen card | The owner only sees it at `/card/pair` and on the card; no mail, no message is sent | Add notifications |
| Making cards one by one | Flashing, Wi-Fi setup, pairing and writing the NFC tag are all manual | Acceptable for a few cards; for dozens, a flashing jig and batch tag writing |
| A 408 from a proxy | Some proxies send an unrequested 408 before closing an idle connection, and the card takes it as "payment did not go through" | Not seen on the path to the production site; not handled |

## Level 3: real money

**What is missing here is mainly not engineering work but decisions.** Today a confirmation only records one row
on the ClawHunt server; no wallet, escrow or ledger entry is written, and the server's tests pin this. Turning a
confirmation into a transfer means changing the server's money path. To be decided before any work starts:

- Where the money comes from and where it goes. The holder's balance on ClawHunt? Who may be a payee? Today anyone
  who opens the card's page can start a request.
- How refunds, disputes, reconciliation and audit work; who sets the limits.
- Compliance. Taking and making payments on behalf of others may need a payment licence in some places. That is a
  question for a lawyer; no conclusion is drawn here.

On the card's side, the firmware guide and the server's operations notes both list these as "before real money":

- **A device key.** Today the server recognises the card by its device token alone, and the token is in unencrypted
  flash. The card needs a key that can sign, a signature with every confirmation, and the server has to check that
  "this confirmation really came from this card". This depends on the flash encryption and secure boot of the level
  above.
- **What the secret is worth.** There are only 6,561 possible secrets. With five tries before the card freezes,
  that stops someone who picked the card up; it is not a key, and it is not meant to stand in for one.
- **The owner's controls.** Setting the secret, unfreezing and setting the no-secret amount need only a signed-in
  session today. Whether to ask for the password again or use a passkey before money moves has to be decided.
- **NFC.** The card carries an NTAG213 that is not connected to the controller; its content can be copied, and it
  cannot prove that "the real card was tapped". As long as every confirmation is made on the card, this is not a
  hole; for the tap itself to count, the hardware has to change.
- **Being watched.** People nearby can see the 8 key presses. Whether large amounts should be confirmed on a phone
  with a fingerprint or face instead has not been done.

## Level 4: selling it as a product

- **The hardware and its marks belong to FoloToy.** The firmware base is under the MIT licence and may be changed and
  used; but the card itself is FoloToy's product, and the upstream brand notes ask that renders keep its marks.
  Selling or handing out the hardware under the ClawHunt mark needs an agreement with FoloToy first. This is not
  legal advice.
- **Certification follows the hardware.** Radio type approval and the like belong to FoloToy's product; selling it
  under another mark or with changed hardware means looking at this again.
- **Nothing that production needs exists yet:** flashing and tag-writing stations, factory tests, packaging,
  returns.
- **Limits of the hardware itself:** three keys, no camera and no fingerprint reader; the NFC tag is not connected
  to the controller; 8 MB of flash, of which 3 MB are for the application, 77% of it used today.

## Suggested order

1. Do the four verifications with the real card against the production site. No money moves; half a day.
2. Decide the money model. This is a product and compliance decision; no code before it is made.
3. Make a "release firmware" on a card you no longer need: compile out the debug commands, turn on flash encryption
   and secure boot, add the device key.
4. Power saving and remote updates.
5. Talk to FoloToy about supply and marks.

After the first two steps the card can be shown as an honest demonstration; only after the third is it worth
discussing letting it touch money.

## What the checks in this repository show

- The application image on the reference card is a build of `firmware/` as released. (For 0.10.0, an image built
  again from the repository differed from the one on the card only in build stamps.)
- `tools/flash.py` is built to write only the application partition, and only to a card whose partition table is
  the factory layout, entry for entry. The tests fill every byte outside the partition table with non-empty data
  and compare byte for byte after flashing; and in Espressif's ESP32-C3 emulator, backup, flash and restore were
  run on a real card's factory flash, with the results at the end of `docs/REPLICATE.md`.
- On a real card it did what it is built to do, once: on 2026-11-25 `flash` wrote 0.11.0 onto the reference card.
  It took the whole-flash backup and had the chip confirm it, wrote the application, and the chip confirmed what
  was written. The whole flash, read again afterwards and compared with that backup: everything outside the
  application partition is the same byte for byte except `nvs`, where the firmware itself stores things; the
  application partition begins with the image. The card came back paired, with its Wi-Fi and its identity
  (recorded at the end of `docs/REPLICATE.md`).
- The factory application can be put back: the backup is of the whole flash, and `restore` writes only its
  application back to where it was. This was run in the emulator, not on a real card; and what this firmware
  saved in `nvs` (Wi-Fi networks, the device token) stays on the card after it.

What they do not show:

- **`restore` has not been run on a real card, and `flash` has run on one card, once.** One run on one card says
  little about other cards, cables and computers.
- The emulator is not a real card: it does not run writes through esptool's helper, so that run used the chip's own
  loader (`--no-stub`); nor does it have the real card's USB port.
- If a different batch of AI Passports has a different partition table, the tool stops rather than forcing the
  write; that card has to be looked at first.
