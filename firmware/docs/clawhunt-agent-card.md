# ClawHunt Agent Card firmware

This guide is for engineers who build, flash, test against or extend the ClawHunt Agent Card firmware. It describes
firmware 0.11.0 (`CARD_FW_VERSION` in `firmware/main/card_model.h`). Paths that start with `main/`, `tests/`,
`assets/` or `components/` are inside `firmware/`; every other path is relative to the repository root, so
`tools/flash.py` is the repository's flashing tool and `firmware/tools/` holds the firmware's own scripts.

## What it is, and what it is not

The firmware turns a retail FoloToy "AI Passport" badge into a physical card for one AI agent on the ClawHunt
marketplace (`https://clawhunt.store`). The card:

- shows the agent's lifetime earnings as the screen's one number, with its deliveries, settled deals and success
  rate, what it earned on each of the last seven days as bars, and its level and name;
- says what the agent is doing (bidding, working, delivered, on hold, disputed) and rings for a new task, a payout
  and a level-up;
- pairs itself with an account on the site through a device-code flow (a code on the screen, typed at
  `/card/pair`);
- shows pay requests that someone opened against the card (for example by tapping its NFC tag, which opens
  `/c/<public_id>`), and lets the holder confirm one by holding OK and typing an eight-press secret.

What it is not:

- **It moves no money.** A confirmation is recorded by the ClawHunt server and nothing else happens. The card holds
  no wallet and no agent key; it cannot bid, accept or deliver for the agent.
- It is not a hardware wallet. Nothing on the card proves to the server that a confirmation came from this card
  rather than from a copy of its device token (see "Known limitations").
- It is not a product. The hardware and its brand are FoloToy's. The firmware is built on FoloToy's upstream
  firmware repository `FoloToy/ai-passport` (MIT).

The server side is the ClawHunt server, which is not part of this repository. This guide describes only what the
card sends and what it reads from the answers.

## Hardware and partitions

| Item | Value |
| --- | --- |
| SoC | ESP32-C3 (RISC-V, no PSRAM) |
| Flash | 8 MB |
| Screen | 240 x 320, 16-bit colour (RGB565); the BSP masks the frame to a 30 px corner radius |
| Keys | Up, Down, OK. A press is reported on key-down; a hold is reported after 500 ms (`BSP_BTN_LONG_PRESS_MS`) |
| Audio | codec and a small speaker |
| NFC | an NTAG213 tag on the back. It is not wired to the MCU; the firmware cannot read or write it |
| USB | the ESP32-C3's native USB Serial/JTAG (console and host link) |
| Radio | 2.4 GHz Wi-Fi only |

`firmware/partitions.csv` is the factory layout of a retail unit, entry for entry:

| Name | Type | Offset | Size | Written by this firmware |
| --- | --- | --- | --- | --- |
| `nvs` | data/nvs | 0x9000 | 0x6000 | yes, only in its own namespace `clawcard` |
| `phy_init` | data/phy | 0xf000 | 0x1000 | no (`CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n`) |
| `factory` | app | 0x10000 | 0x300000 | this is the application |
| `imgstore` | data/spiffs | 0x310000 | 0x20000 | no |
| `imgframe` | data/spiffs | 0x330000 | 0x26000 | no |
| `cardid` | data/nvs | 0x356000 | 0x4000 | no: the unit's identity |
| `imguser` | data/spiffs | 0x35a000 | 0x20000 | no |
| `audio` | data/spiffs | 0x37a000 | 0x80000 | no |
| `imgava` | data/spiffs | 0x3fa000 | 0x100000 | no |

The Wi-Fi driver keeps its state in RAM (`CONFIG_ESP_WIFI_NVS_ENABLED=n`), so the factory firmware's saved
network in `nvs` survives. The 0.11.0 application is about 2.43 MB, 77 % of the 3 MB partition.

Everything the firmware stores is in the NVS namespace `clawcard`:

| Key | Content |
| --- | --- |
| `ssid`, `psk`; `ssid1`..`ssid3`, `psk1`..`psk3` | up to four saved Wi-Fi networks, newest first (the first pair keeps the key names of older firmware) |
| `snd` | `1` when the holder switched sound on in the card's menu |
| `api` | the server address when it is not the site; absent for `https://clawhunt.store` |
| `token`, `tcur` | the device token and task cursor for the site |
| `token2`, `tcur2` | the same for any other server the card is pointed at |
| `week`, `week2` | what the agent earned on each of the last seven days, as the card saw it, and the highest total it was told, for the site and for any other server (see "The week of earnings") |

All of these are stored in plain text: flash encryption is not enabled.

## Build

The firmware is built with ESP-IDF 5.5.3 for `esp32c3`. The project name stays the upstream's,
`FoloToy-AI-Passport`, so the build output is `firmware/build/FoloToy-AI-Passport.bin`.

```bash
./build.sh            # the idf.py on the PATH (it must report 5.5.3, or the build stops); Docker if there is none
./build.sh --docker   # Espressif's ESP-IDF 5.5.3 container, pinned by digest in build.sh
```

On Windows without Docker, with a local ESP-IDF:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -IdfPath <esp-idf folder> -IdfToolsPath <tools folder>
```

`build.ps1` clears the `MSYSTEM` variable (set by Git Bash), which otherwise makes ESP-IDF refuse to run. Both
scripts read the version from `CARD_FW_VERSION`, pass it as `PROJECT_VER` (so the image's app descriptor carries it),
and copy the image to `dist/clawhunt-agent-card-<version>-app.bin` with a line in `dist/SHA256SUMS.txt`. A
`firmware/build` folder made by the other way of building, or on another machine, has to be removed first.

The image is the application only. There is no merged image to flash, and none should be made for a card.

Before the card is paired it shows a default agent id and name compiled into `main/card_identity.h`. To build a card
that shows your own agent before pairing, regenerate that header and rebuild:

```bash
python firmware/tools/bind_identity.py --agent <agent id>   # reads GET /api/agents/<id>; public data only
```

## Flash

Only the application partition at `0x10000` is ever written. Use `tools/flash.py` (Python 3.9+, esptool 5;
`pip install -r tools/requirements.txt`):

```bash
python tools/flash.py check                    # read only: chip, flash size, partition table, current firmware
python tools/flash.py plan  <app image>        # say what `flash` would do
python tools/flash.py flash <app image>        # back up the whole flash, then write the app to 0x10000
python tools/flash.py restore <backup.bin>     # write a backup's application back (usually the factory one)
```

`flash` checks the image (checksum, embedded SHA-256, project name), checks that the chip is an ESP32-C3 with 8 MB
and that its partition table equals the factory layout entry for entry, reads and verifies a full backup (unless
`--no-backup` is given), asks for `yes`, checks that the same card is still on the port, then writes. Options:
`--port`, `--yes`, `--backup-dir`, `--no-backup`, `--no-stub`, `--any-image`, `--expect-version`.
`docs/REPLICATE.md` walks through the whole procedure, including Wi-Fi, pairing, setting the secret and writing
the NFC tag.

Do not flash a merged image from `0x0` as the upstream instructions do: that erases `cardid`, which only a
whole-flash backup can bring back (so do not skip the backup on a card that has none yet). The backup contains the
unit's identity, its Wi-Fi passwords and its device token; keep it private.

## The card's states and screens

The card has one resting face and a set of full-screen sheets. Only one sheet is up at a time.

Every screen stands on one dark ground and is set from the left edge. One colour, mint, is kept for the screen's
one number, for today's bar and for what is live. Amber says the card cannot pay yet or that something did not go
through; red is used for one thing only, a frozen card. Two sheets leave the dark ground: a payment the server
took and a payout are set in the ground's colour on a whole screen of mint. A sheet names itself in a tag at the
top left, and its one number stands where the face's number stands.

Latin and digits are set in Cascadia Mono, smoothed; Chinese in the 12 px pixel font, at 12 and 24 px. The labels
of the face are English capitals; sentences are Chinese. The tables give the meaning and the original words.

### The face

| Part | What it shows | Where it comes from |
| --- | --- | --- |
| Header, left | the crab mark and `CLAWHUNT` | fixed; the mark moves faster while the agent is busy |
| Header, right | one word for what the agent is doing, with a dot: `IDLE`, `BIDDING`, `WORKING`, `DELIVERED`, `ON HOLD`, `DISPUTE`; `NO SECRET` in amber, `LOCKED` in red; `CONNECTING` or `OFFLINE`, dim and with a hollow dot, while nothing tells the card (no network, no computer) | the task poll and the card's own view (`security`); the network state |
| The number | the agent's lifetime earnings in USD, cut to three figures with `K` or `M` (`1.92K`; cut, never rounded up), under the label `TOTAL · EARNED` | `total_earned` |
| Under it | the exact figure (`$1,923.32`), then up to three facts: deliveries, settled deals, success rate (`55 deals · 39 settled · 100%`). A fact that is not known is left out | `total_earned`, `total_tasks_completed`, `settled_total`, `success_rate` |
| In place of the facts | what stops the card from paying, in amber or red: no secret set (`没设口令，还不能付款`), frozen (`卡已冻结`), frozen by wrong secrets (`口令输错，卡已冻结`). Any key then says where to fix it (`/card/pair`) | `security` |
| The week | seven bars, one per day, today on the right in mint, with the days' letters under them. A day without a figure is a mark on the baseline | kept by the card itself, see "The week of earnings" |
| Foot | `LV`, the level, a bar towards the next level, and the agent's name (cut with an ellipsis when it is long) | `level`, `total_tasks_completed`, `name` |
| Foot, over it | one line: what the crab says (1.4 s), a state the agent has just moved into, said once in its own words (`中标了，开工`), or a hint that stays: `长按 OK 键连 Wi-Fi` while no network is saved | the state machine |

Until the card has been told its agent's numbers (it has just started, or there is neither a network nor a
computer), the number is `--` under the label `NO DATA YET`, there are no facts, the week has no figures and the
level is a dash: the card does not show `0.00` for an agent it knows nothing about yet.

Keys on the face: OK, the crab says a line; Up or Down, it says how to open the settings; hold OK (500 ms), the
settings.

### The sheets

| Screen | Shown when | Keys |
| --- | --- | --- |
| Pairing code (`绑定这张卡`) | unpaired and online | the code in two lines, the page to open, minutes of validity. It is the resting screen until paired; it closes when the code is dead and no new one came |
| New task, from the server (`新单`) | the server reports a bid or a direct assignment | any key closes it (`按 OK 知道了`); it closes by itself after 5 to 30 s |
| New task, from a computer | USB `offer` | OK: take it (card answers `accept`, status `竞标中…`); timeout: "too slow" sheet |
| Too slow (`手慢了`) | a computer's offer timed out | none; 1.6 s |
| Payout (`到账`) | the server or a computer reports a payout | none; 2.4 s, then level-up if the payout carried a new level |
| Level up (`升级了`) | after a payout with a new level | none; 2.2 s, large animated crab |
| Pay request (`付款请求`) | the server (or a computer) asks to pay | hold OK: approve (or open secret entry); Up/Down: decline; OK press: "keep holding" hint. Keys in the first 0.8 s are ignored |
| Secret entry (`输入口令`) | holding OK on a request that needs the secret | see "Pay requests and the holder's secret" |
| Sending (`确认中`) | an approval is on its way to the server | none; up to 15 s, then "no confirmation received" (`没收到确认`) |
| Confirmed / paid (`已确认` / `已付款`) | the server accepted (or a local USB request was approved) | none; 2.4 s |
| Not paid (`没付`) | declined or timed out | none; 1.5 s |
| Failed (`没付成`, `口令不对`, `卡已冻结`, `没设口令`) | the server refused an approval | none; 2.4 s, or 5 s for frozen and no secret |
| Settings (`设置`) | hold OK at rest | Up/Down select, OK confirm. Items: connect Wi-Fi, sound on/off, back. Closes after 15 s idle. Shows agent number and firmware version |
| Phone setup | see "Wi-Fi provisioning" | any key leaves setup |

The agent's task phase on the face: the header's word, and the line that is said once at the foot when the agent
moves into the phase.

| Phase | Header | Said once | Crab |
| --- | --- | --- | --- |
| `bid` | `BIDDING` | `竞标中…` (bidding) | busy |
| `won` | `WORKING` | `中标了，开工` (won, starting) | busy, blip |
| `own` | `WORKING` | `收到，这就开工` (assigned directly, starting) | busy, blip |
| `work` | `WORKING` (a computer's job adds its percentage) | `干活中…` (working; no percentage from the server) | busy |
| `done` | `DELIVERED` | `交付了，等验收` (delivered, waiting for acceptance) | busy |
| `held` | `ON HOLD` | `验收了，等放款` (accepted, payout on hold) | resting |
| `disputed` | `DISPUTE` | `有争议，等裁定` (dispute open) | resting |
| `lost` | `IDLE` | `这单没中` (not picked) or `这单结束了，没有到账` (ended without payout) | resting |
| none | `IDLE` | nothing | resting |

### The week of earnings

The server tells the card one number about its agent's money, the lifetime total, about once a minute. The card
has no clock, but every answer of the server carries the time (the HTTP `Date` header). From the two the card
keeps a week (`main/week.c`): when the total has grown between two answers that are at most fifteen minutes
apart, the growth is today's.

Where the card cannot tell, it leaves out rather than adds:

- What the card did not see it does not make up. A day on which the card got no answer has no figure, and what
  was earned while the card was off, out of reach or restarting is in the total and in no day. The first answer
  after a restart only says where the total stands.
- Only what rises above the highest total the card was ever told counts. A total that fell and came back (a
  correction on the server's side, one answer with a wrong figure) has earned nothing, and after a correction
  downwards the bars stay empty until the total has passed its earlier height. An answer that does not say the
  total at all is not read as zero: it leaves the week as it is.
- A card that has just been given this firmware therefore starts with an empty week, and the bars grow from that
  day on.
- A day begins at midnight of UTC+8 (`CARD_UTC_OFFSET_MIN` in `main/card_model.h`); the card has no setting for it.
- An answer without a `Date` header leaves the week as it is. The header is read in the one form servers send
  today (`Wed, 25 Nov 2026 01:45:16 GMT`), for the years 2024 to 2099.
- An answer that is dated oddly costs little and adds nothing. Nothing is measured across an answer dated before
  the one before it. A single answer dated before the day the card keeps is ignored; two of them in a row, close
  together, mean that the kept day never came (a server whose clock ran ahead once), and the week moves back to
  the day they give.
- The week is written to flash only when what is kept of it changes (a day's figure, the day, the highest total),
  as `week` for the site and `week2` for any other server (agent 1 of a test server is not agent 1 of the site).
  Another agent's numbers start a new week.
- A computer can put a week on the face with the USB `stats` message (`week`, `wd`); that is not stored.

A task is not a mode of the card: pay requests, settings and setup work during it.

Ordering rules for sheets from the server: if the screen is taken (menu, setup, a pay request, another sheet),
payouts wait (up to three, oldest first) and the newest new-task sheet waits (shown only within one minute of
arrival). A pay request takes precedence: a server payout or new-task sheet that is up gives way and comes back
afterwards. A computer's `offer`, `paid` and `job own` also go before the server's sheets. A pay request that
arrives while the card is not at rest is declined at once with reason `busy`.

## Identity and pairing

Pairing follows the shape of the OAuth device authorization grant (RFC 8628):

1. Unpaired and online, the card asks for a code (`POST /api/cards/pairing`) and shows `user_code` with the
   `verification_uri` (for example `clawhunt.store/card/pair`).
2. The account owner signs in on that page, types the code and picks one of their agents.
3. The card polls `POST /api/cards/pairing/token` with its `device_code` until it receives a `device_token`,
   stores it in NVS and starts using it.
4. An unpairing on the site makes the server answer 401; the card then drops the token and shows a new code.

Before pairing, the card shows the built-in agent's public profile. After pairing, the face shows the agent the
server returns, and the plate number follows that agent's id.

## The server API the card uses

The default server is `https://clawhunt.store`. USB `api` can point the card at another server. Plain `http` would
carry the device token in clear, so it is accepted only for an address whose text begins with `http://192.168.`,
`http://10.`, `http://127.` or `http://172.16.` (for example `http://<lan-address>:<port>`). This is a check of
the beginning of the address as it is written, meant for a test server on a private network; it is not a full
check of the private address ranges.

Every request:

| Header | Value |
| --- | --- |
| `User-Agent` | a browser-like string ending in `ClawHuntCard/<firmware version>` (a CDN in front of a server may turn away clients that do not look like a browser) |
| `Accept` | `application/json` |
| `Content-Type` | `application/json`, on requests with a body |
| `Authorization` | `Bearer <device_token>`, only on device requests while paired |
| `X-Card-Boot` | eight hex digits drawn at power-on, kept only in RAM; sent with `Authorization` |

Redirects are not followed. The request timeout is 12 s. Answer bodies beyond 3071 bytes are cut. The card runs one
HTTP client on one task, so requests never overlap; the loop wakes every second.

### Requests

**Ask for a pairing code**

| | |
| --- | --- |
| Request | `POST /api/cards/pairing`, no token, body `{"fw":"0.11.0","agent_hint":<built-in agent id>}` |
| When | unpaired, online, not in setup; when there is no code or it has expired |
| Read on 200 or 201 | `device_code` (under 96 bytes), `user_code`, `expires_in` (s, default 600, clamped 60..3600), `interval` (s, default 5, clamped 2..30), `verification_uri` (shown without its scheme) |
| Any other answer, or a missing field | no code; retry after 15 s, doubling up to 10 minutes |

**Poll the pairing code**

| | |
| --- | --- |
| Request | `POST /api/cards/pairing/token`, no token, body `{"device_code":"<device_code>"}` |
| When | every `interval` until paired or the code expires |
| 200 with `device_token` (under 80 bytes) | paired: the token is stored, the face's task state and cursor start fresh |
| 4xx with `error` = `authorization_pending` | keep polling |
| 4xx with `error` = `slow_down` | interval + 5 s, each time, without an upper limit |
| 429 | interval + 5 s as long as it is under 30 s; the code is kept |
| 404, or 4xx with any other `error` | the code is dropped; a new one is asked for |
| 4xx without `error` (for example a CDN's 403 challenge page), 5xx, no answer | the code is kept; poll again at the same interval |

**Public profile (unpaired)**

| | |
| --- | --- |
| Requests | `GET /api/agents/<built-in id>`, then `GET /api/agents/<built-in id>/track-record` |
| When | every 60 s after a success, 15 s after a failure |
| Read | from the agent: `name` (required), `id`, `level`, `total_tasks_completed`, `success_rate` (0..1), `total_earned` (cents). From the track record: `settled_total`, found by text search near the start of the body, so a cut body still yields it |

**Device profile (paired)**

| | |
| --- | --- |
| Request | `GET /api/cards/device/me`, with token |
| When | every 60 s after a success, 15 s after a failure; at once after a refusal that says frozen or no secret; within 2 s after a payout |
| Read | `agent` with the fields above plus `settled_total`; `security` with `secret_set` (bool), `frozen` (bool), `frozen_reason` (`attempts` means frozen by wrong secrets) |
| 401 | the token is dropped; the card shows a new pairing code |
| Anything else | keep the face; retry in 15 s |

The face shows: lifetime earnings, deliveries (`total_tasks_completed`), settled count (left out when unknown),
success rate (left out while there are no deliveries), level, and progress to the next level computed from
deliveries with the ladder 2, 5, 10, 20. A missing `security` object (an older server) means the face says
nothing about it. Of both profile answers the card also reads the `Date` header: with `total_earned` it is what
the week's bars are kept from (see "The week of earnings").

**Next pay request (paired)**

| | |
| --- | --- |
| Request | `GET /api/cards/device/pay-requests/next`, with token |
| When | every 4 s |
| Read on 200 | `request` object: `id` (> 0), `payee`, `memo`, `match_code` (shown as a check code the requester's page also shows), `amount_cents` (> 0), `expires_in` (s, default 30), `auth` (`secret` or `none`) |
| Shown | when `id` differs from the last one shown and payee and amount are present. Time on screen: `expires_in` minus 4 s, clamped 5..20 s |
| 401 | the token is dropped |
| Anything else | nothing shown; ask again in 4 s |

**Decision on a pay request**

| | |
| --- | --- |
| Request | `POST /api/cards/device/pay-requests/<id>/decision`, with token |
| Body, decline | `{"approve":false,"reason":"declined"}`, `"timeout"` or `"busy"` |
| Body, approve | `{"approve":true,"reason":"approved"}` |
| Body, approve with secret | `{"approve":true,"reason":"approved","secret":"<8 of U/D/O>","attempt":"<8 hex>"}` |
| Accepted | 200 with `status` equal to `approved` (or `declined` for a decline) |
| Refused | `error`: `wrong_secret` with `attempts_left`, `secret_required`, `secret_not_set`, `card_frozen` (or status 423). Anything else without a known `error`: shown as "not accepted, may have expired" |
| Retries | up to three tries in all. No answer, 403, 429 and 5xx count as not answered and are sent again (after 0.7 s, or at once after no answer). Any other status is the server's answer and is not repeated |

A 401 on a decision does not unpair the card; it is shown as a refusal. Declines are sent the same way, with the
same retries, but the card does not wait for their result. The server must treat a repeated identical decision as
one, and a repeated `attempt` id as one try of the secret.

**Agent tasks (paired)**

| | |
| --- | --- |
| Request | `GET /api/cards/device/tasks/next`, or `...?cursor=<cursor>` once the card has one; with token |
| When | every `poll_s` seconds (default 15, clamped 5..300) |
| Read on 200 | `job`, `event`, `cursor` (required: 1..95 characters from `0-9 A-Z a-z . - _`), `poll_s` |
| `job` | `id`, `phase` (`bid`, `won`, `own`, `work`, `done`, `lost`, `held`, `disputed`), `why` (`called_off` for a lost task that ended without payout). Missing or null: nothing in hand. An unknown phase leaves the line as it is |
| `event`, kind `new_task` | `id`, `title`, `private` (no title: the card prints its own words), `amount_cents` (0: shown as `--`), `direct` (someone asked for this agent by name; otherwise the agent itself bid), `ttl_s` (default 20, clamped 5..30) |
| `event`, kind `paid` | `id`, `title`, `private`, `net_cents`, `level` (non-zero: the new level) |
| Other event kinds | ignored |
| 401 | the token is dropped |
| 404 | the server has no such route; ask again in 10 minutes |
| Anything else | ask again at the current interval, with the old cursor |

### Rules

- **Token storage.** The site's token is kept as `token`, any other server's as `token2`. Pointing the card at
  another server starts it unpaired there; the site's token stays in flash, unused and never sent to the other
  server. Pointing it back (`api` with `""` or the site's address) resumes the site's pairing without a new code.
  Leaving another server erases its token and cursor.
- **Only 401 ends a pairing**, and only on the profile, pay-request and task polls. A 403 is taken to come from
  something in front of the server (a CDN's challenge page, for example), not from the server, and is retried like
  any failure.
- **Boot id.** `X-Card-Boot` changes on every restart. The server uses it to tell a restart from a continuing run.
- **Cursor.** The server keeps no state for the card. The cursor in each answer stands for what the card has been
  told; the card sends it back unchanged. It is written to flash only when it changes, and only after the screen
  has accepted the whole answer; an answer the screen could not take is asked for again with the old cursor.
  Unpairing, re-pairing and a change of server discard the cursor and every sheet still waiting.
- **Reused connection.** The card keeps its TLS connection between requests. When a request fails within 3 s on a
  connection that was already open, before any response header arrived, the card drops the connection and sends the
  same request once more on a new one. Timeouts, failures on a fresh connection and requests that received any
  answer are not resent by this rule.
- **During phone setup** the card makes no requests and releases the TLS client to free memory.

What a server should send for each status, as the card reads it:

| Status | Pairing poll | Profile / pay / task polls | Decision |
| --- | --- | --- | --- |
| 200 | token or nothing new | data | accepted if `status` matches |
| 400 with `error` | depends on `error` | failure, retry later | refusal by `error` |
| 401 | treated as other 4xx | unpair | refusal |
| 403 | keep the code | failure, retry later | not answered: resend |
| 404 | drop the code | failure (tasks: wait 10 min) | refusal |
| 423 | other 4xx | failure | frozen |
| 429 | slow down | failure | not answered: resend |
| 5xx, no answer | keep the code | failure | not answered: resend |

## Pay requests and the holder's secret

A pay request is shown with the payee (`付给 ...`), the amount in USD, the check code (or, when the request has none, the memo), and the note "trial:
only recorded, no charge" (`试运行：只记录，不扣款`). Approving always takes a held OK, so a brush against the key
does not approve; a short OK only changes the hint to "keep holding" (`继续按住…`).

When the request says `"auth":"secret"`, holding OK opens the secret entry instead of approving:

- The secret is eight presses of the three keys, Up (`U`), Down (`D`) and OK (`O`), in any order: 3^8 = 6561
  possibilities. An example of the form is `UUDDOUDO`; it is an example only.
- It is set and changed by the account owner at `/card/pair`. The card never stores it and does not know the right
  one. The server checks it.
- Each press fills one of eight squares; the screen never shows which key was pressed, and the USB link reports
  such presses as `"k":"*"`. Presses in the first 0.4 s after the sheet appears are ignored.
- After the eighth press the card waits 0.8 s (a hold is only recognised after 500 ms, and starts as a press), then
  sends the approval with the secret and a fresh random `attempt` id.
- Hold Down: start again. Hold Up: decline. Hold OK: counts only as the press it began with.
- The request's own time keeps running; when it ends the request times out and what was typed is wiped.
- The typed presses live in one buffer from the first press until they are handed to the cloud task, which copies
  them into one request body and wipes the copies the firmware owns after sending (what the HTTP client and the TLS
  layer keep in their own buffers is outside its reach). They are never written to flash, logged or sent over USB.

Answers, as the card shows them:

| Server answer | Card |
| --- | --- |
| accepted | confirmed |
| `wrong_secret`, 4 s or more left | entry opens again, with "wrong secret, N tries left" (`口令不对，还能试 N 次`) |
| `wrong_secret`, less than 4 s left | "wrong secret"; the request is void; N tries left |
| `secret_required`, 4 s or more left | entry opens (a server that changed its mind after the request was shown) |
| `secret_required`, less than 4 s left | not paid: not enough time to type the secret |
| `card_frozen` / 423 | "card frozen" for 5 s; the resting face keeps saying it; the profile is fetched at once |
| `secret_not_set` | "no secret set" for 5 s; the resting face keeps saying it |

The rules about attempts are the server's, and the card only displays what it is told. As the server behaves
today: five wrong secrets freeze the card; the count is kept on the server, so a restart does not clear it; the
owner can freeze and unfreeze the card on the site; and small amounts can be allowed without the secret (off by
default). The card learns that only per request, through `"auth":"none"`. When the secret is required, and how
attempts are counted, is decided by the server and may change. A request with no `auth` (an older server) is
approved by a held OK alone.

## What the card is told about its agent's tasks

The task route is read-only. The card holds no agent key: a new-task sheet appears after the agent already bid with
its own key (the line says `你的 agent 出价了` and the large number is its bid) or after a buyer assigned the task to
it by name (`有人点名要你`, the large number is the task's amount). Any key only closes the sheet, and over USB the
card answers `{"t":"seen","id":...}`; there is no "too slow" sheet for these.

After an absence (the card switched off), the server's answers bring the card to the current phase without replaying
the steps in between. As the server behaves today, payouts from the last seven days are shown (at most the last
three, oldest first), a task that disappeared is mentioned once, and a new task rings only if it is less than 15
minutes old.

## Wi-Fi provisioning

The holder starts it from the card: hold OK, choose `连 Wi-Fi`, press OK. A computer can start it with USB `setup`.

1. The card opens a WPA2-PSK hotspot named `ClawCard-XXXX`, where `XXXX` is the last two bytes of its station MAC
   in hex. The password is 12 characters drawn at random for every session from 31 characters that are hard to
   misread (about 59 bits), without modulo bias, after the radio is started.
2. The screen shows a QR code in the standard `WIFI:T:WPA;S:<name>;P:<password>;;` form, the hotspot name and its
   password. A phone camera joins with it.
3. DHCP option 114 and a DNS server that answers every name with the card's address make the phone open the setup
   page; otherwise the holder opens `http://192.168.4.1/`.
4. The page lists networks, takes the password and shows the same phase as the screen: joined, trying, done,
   failed. It polls the card every 1.5 s.
5. A network is saved only once the card got an address on it, so a wrong password does not replace a saved network.
   After success the card closes the hotspot 6 s later. Any key on the card leaves setup; so does five minutes
   without use (not while a network is being tried). A try gives up after 30 s.

If a try fails and the phone has dropped off the hotspot, the card shows the QR code again with the reason (wrong
password, network not found, other).

Saved networks: up to four, newest first; when the table is full the oldest goes. With one saved network the card
joins it directly (this also finds a hidden network); with several it scans and joins the strongest saved one; if
none is in range it looks again every 30 s.

The page's API, all on `http://192.168.4.1`:

| Request | Answer |
| --- | --- |
| `GET /` | the page (embedded from `main/setup_page.html`) |
| `GET /api/state` | `{"card","phase":"idle|trying|ok|failed","reason":"|password|notfound|other","ssid","online","saved":[{"ssid","id"}]}` |
| `GET /api/scan` | `[{"ssid","id","rssi","open"}]`, up to 12 names, strongest first, each name once; 409 `{"busy":1}` while trying; 503 when the scan failed |
| `POST /api/join` | body `{"id":"<hex>"}` or `{"ssid":"..."}` plus `"psk"`; 200 `{"ok":1}`, 400 `{"bad":1}`, 409 `{"busy":1}` |
| `POST /api/forget` | body `{"id"}` or `{"ssid"}`; 200 `{"ok":1}`, 404 `{"bad":1}` for a network that is not saved, 400 `{"bad":1}` for a body that is not JSON |
| anything else | 302 to `http://192.168.4.1/` (answers the phones' captive-portal probes) |

Security measures:

- The hotspot is WPA2 with a per-session random password shown only on the screen. No USB message carries it and
  it is not logged, and it is cleared when setup ends; a screenshot taken over USB while that screen is up does show
  it, like anything else on the screen. The home network's password therefore never travels over an open network.
- The page and its API answer only connections that arrived on the hotspot's own address; connections from the
  network the card has joined are closed as they open (403 if one gets through). DNS listens on the hotspot only.
- The API answers only `Host: 192.168.4.1` (or with `:80`). During setup every name resolves to the card, so without
  this check a page from any other site would share the API's origin.
- The test message USB `setup` with `"lan":1` switches the two checks above off, so that the page can be driven
  from the network the card has joined. It is one of the messages a production build must not have.
- Writes accept only `application/json`, bodies of at most 256 bytes. A request with a larger body, or a body on a
  method that takes none, is answered 413 and the connection is closed instead of reading the body.
- The page never returns a saved password. Request bodies are zeroed after parsing.
- Network names are also sent as hex of their raw bytes (`id`), and the page displays names with `textContent`, so a
  name that is not valid UTF-8 can still be chosen and cannot inject markup.
- cJSON's nesting limit is 8 for this build (`CJSON_NESTING_LIMIT` in `main/CMakeLists.txt`), down from 1000, so a
  body or USB line of deeply nested brackets cannot exhaust the parsing task's stack.
- At most two clients can join the hotspot; the HTTP server keeps at most four sockets with 5 s receive and send
  timeouts.
- `components/dns_server` comes from ESP-IDF's `captive_portal` example, changed so that stopping it closes its
  socket (otherwise port 53 stays taken on the next session).

Not covered: the page is plain HTTP, so another device on the card's own hotspot sees the same page. Whether the
hotspot's "station connected" event fires only after the WPA2 handshake was not established from the source; a
wrong hotspot password did not make the card report a joined phone.

## USB host link

The link is the ESP32-C3's USB Serial/JTAG port (Espressif VID `0x303A`). Keep DTR and RTS low when opening it:
raising them resets the chip. One message per line in both directions: `@` followed by a JSON object. Lines that do
not start with `@`, are not a JSON object, have an unknown `t`, fail validation or exceed 383 bytes are dropped.
Numbers are clamped to their ranges, and text is cut on a UTF-8 character boundary (titles 63 bytes, names 23).
Money is in cents and ratings are times ten; no floats cross the link. Once it has started, the card writes log
lines only at warning level and above on the same stream (the lines of the start-up itself are more detailed).

None of these messages is authenticated. There is no build option that removes them today. A production build must
compile out at least every message marked "test" below; the readiness notes suggest keeping only a read-only `ping`.

### Computer to card

| Message | Fields | Effect | Use |
| --- | --- | --- | --- |
| `stats` | `name` (required), `id`, `lvl`, `xp` (0..100), `deals`, `rating` (x10, 0..50), `streak`, `today` (cents: the face's number), `money_label` (what the number is; default `TODAY · EARNED`), `cells` (three `[label,value]` pairs: the facts under the number), `week` (seven numbers, cents per day, today last, below zero for no figure) with `wd` (today's day of the week, 0 = Monday) | sets the face; outranks the card's own profile for 90 s. The week is shown, not stored | gateway feed |
| `offer` | `id`, `title` (required), `amt`, `direct` (0/1), `ttl` (ms, 1000..30000, default 6000) | an offer the holder can take with OK | gateway feed |
| `offer` + `"remote":1` | as above | plays the server's new-task sheet | test |
| `job` | `s`: `bid`, `won`, `own`, `work`, `done`, `lost`, `held`; `p` (0..100, for `work`) | the computer's job flow; while working the card takes no other offer | gateway feed |
| `paid` | `id`, `title`, `net` (cents), `lvl` (new level, or 0) | payout sheet | gateway feed |
| `paid` + `"remote":1` | as above | plays the server's payout (waits for the screen) | test |
| `task` | `id` (0 clears), `s` (the eight phases incl. `disputed`), `why` (`called_off`) | plays the server's task phase | test |
| `pay` | `id`, `payee` (required), `amt` (> 0), `memo`, `code`, `bal` (cents; -1 = no money, trial note), `ttl` (ms, 5000..60000, default 20000) | a local pay request; the card answers `pay_ok`/`pay_no` itself | gateway feed |
| `pay` + `"remote":1` | as above, optional `"auth":"secret"` | plays a server request; the computer answers with `pay_result` and the card's server is never told | test |
| `pay_result` | `id`, `ok` (0/1), `why` (`wrong_secret`, `need_secret`, `frozen`, `no_secret`), `left` | plays the server's verdict; only ends a request that came over USB | test |
| `guard` | `secret` (0/1), `frozen` (0, 1 by the owner, 2 by wrong secrets), `known` (0 forgets) | plays the server's `security` | test |
| `pairing` | `code` (required), `where` (default `clawhunt.store/card/pair`), `min` (1..60, default 10) | shows a code the server never issued | test |
| `pairing` + `"gone":1` | | plays "code dead, no new code": the sheet closes | test |
| `paired` | | clears a shown code, says "paired" | test |
| `setup_view` | `phase`: `joined`, `trying`, `done`, `failed`, `off`; `ssid`; `reason` (0..3; with `off`, 1 = "Wi-Fi connected") | display only; never the screen with the hotspot password | test |
| `say` | `text` | the crab says it, at rest | gateway feed |
| `key` | `k`: `up`, `down`, `ok`; `"long":1` for a hold | presses a key (not counted as a physical key) | test |
| `ping` | | the card answers `hello` | keep |
| `sound` | `on` (0/1) | sound on or off for this boot, not stored | gateway feed |
| `wifi` | `ssid`, `psk` | saves a network and joins it | setup over USB |
| `wifi` + `"scan":1` | | scans; result arrives as `scan` | setup over USB |
| `wifi` + `"forget":1` | | forgets every saved network | setup over USB |
| `setup` | `on` (0/1), `lan` (0/1) | starts or stops phone setup; `lan` also serves the page on the joined network | `lan`: test |
| `api` | `base` (`""` = the site) | points the card at another server | test |
| `unpair` | | drops the token of the server named in flash; the card shows a new code. On a card paired with the site this is a real unpairing: re-pairing gives a new public id and the NFC tag must be rewritten | test |
| `FAP_SCREENSHOT_V1` | raw word, no `@`, matched on a sliding window | screenshot | test |

`stats`, `job`, `say`, `ping`, and `offer` and `paid` without `"remote":1`, light the face's "computer connected"
dot for 6 s. The other messages, including every message that plays the server, do not.

The screenshot answer is the line `FAP_SCREENSHOT_V1 240 320 RGB565LE 153600` followed by 153,600 bytes of raw
pixels, row by row. The card streams each band as LVGL redraws the screen, after the BSP's corner mask, so the shot
is what the panel shows; logging is silenced while it is sent.

### Card to computer

| Message | Fields | Sent when |
| --- | --- | --- |
| `hello` | `fw`, `agent` (built-in id), `sound`, `wifi` (`off`, `connecting`, `online`, `failed`), `ip`, `nets` (saved networks), `setup` (phase), `cloud` (`unpaired`, `pairing`, `paired`), `api` (server in use), `heap`, `heap_min`, `keys` (physical key events since power-on) | at boot and on every `ping` |
| `accept` | `id` | OK on a computer's offer |
| `miss` | `id` | a computer's offer timed out |
| `busy` | `id` | an offer or a USB pay request arrived while the screen was taken |
| `seen` | `id` | a key closed a server (or `remote`) new-task sheet |
| `key` | `k` (`up`, `down`, `ok`, or `*` during secret entry), `long` | every key event, physical or injected |
| `pay_ok` | `id`; `"secret":1` when a secret was typed for a USB `remote` request | approval of a USB request |
| `pay_no` | `id`, `why` (`declined`, `timeout`) | decline or timeout of a USB request |
| `sound` | `on` | after `sound` |
| `wifi` | `ok`, `err` (ESP-IDF error name), `state` | after `wifi` |
| `scan` | `aps`: `[[ssid, rssi], ...]`, up to 8, weakest dropped to fit the line | after a scan |
| `setup` | `phase` (`waiting`, `joined`, `trying`, `done`, `failed`, `off`), `ap` (hotspot name), `why` | phone setup changed phase. With `failed` or `waiting`: 1 wrong password, 2 not found, 3 other. With `off`: 1 success, 2 hotspot did not start. Never the hotspot password |
| `api` | `ok`, `err` | after `api` or `unpair` |
| `cloud` | `"pairing":"started","code"`; `"pairing":"failed","http"`; `"pairing":"gone"`; `"paired":1,"saved"`; `"token":"rejected","http"`; `"decision":<id>,"approve","http"` | pairing and decision events |
| `net` | `sync` (`ok`, `failed`), `http`, `via` (`public`, `device`) | after each profile fetch |

Lines written while no program reads the port stay in the card's USB buffer and arrive together when a program
opens it. Timestamp them on arrival before drawing conclusions about rates.

### Tools that use the link

- `firmware/tools/feed.py demo` sends made-up offers and payouts (`--gap`, `--auto-accept`); `shot <png>` takes a
  screenshot; `verify <dir>` walks through the screens and saves one image each. It does not talk to ClawHunt.
- `firmware/tools/gateway.py` reads the built-in agent's public profile every 60 s, serves a local "pay this card" page at
  `http://127.0.0.1:8765/c/<id>` (`--lan` to reach it from a phone), keeps a sandbox wallet in a local JSON file with
  a $50 per-payment and $200 daily limit, offers `/setup` to send Wi-Fi credentials from the local machine only, and
  can hold quiet hours (`--quiet-hours`). This is the pre-0.4 way of running the card; the card now talks to the
  server itself.

## Sound

The card boots silent unless the holder switched sound on in its menu (stored as `snd`). USB `sound` changes it for
the current boot only. Each chime is a sine with two quiet overtones, a 4 ms attack and an exponential decay, at
16 kHz:

| Chime | Used for |
| --- | --- |
| blip | key presses, task won or assigned |
| ring | new offer or task, pay request |
| coin | payout, Wi-Fi connected, paired, local payment approved |
| level up | level-up sheet |
| lose | missed, declined, failed, wrong secret |
| paid out | a server request confirmed |

`firmware/tools/sfx_preview.py <dir>` renders the same chimes to WAV files (keep its table in step with `main/sfx.c`). How
they sound on the card's speaker was not assessed.

## Source map (`firmware/main/`)

| File | What it does |
| --- | --- |
| `main.c` | bring-up: display, sound, card app, keys (press and hold), host link, cloud task, Wi-Fi |
| `card_model.h`, `card_model.c` | version, the event and data types shared by all modules; UTF-8-safe copy and money formatting |
| `card_app.h`, `card_app.c` | the state machine: modes, sheets, queueing of server sheets, pay requests, secret entry, menu |
| `cloud.h`, `cloud.c` | all HTTPS: pairing, profiles, pay requests, decisions, tasks, token and cursor storage, boot id |
| `week.h`, `week.c` | the week of the agent's earnings: plain C with no platform calls (the HTTP date, the days, what is kept over a restart) |
| `host_link.h`, `host_link.c` | the USB line protocol, `hello`, the screenshot stream |
| `net.h`, `net.c` | Wi-Fi station, saved networks, scans, the setup hotspot, small NVS store |
| `setup.h`, `setup.c` | phone setup session: hotspot, DNS catch-all, the page's HTTP API |
| `setup_page.html` | the setup page, embedded with `EMBED_TXTFILES` |
| `ui_card.h`, `ui_card.c` | everything drawn: the face, the sheets, the QR code; the colours and every position |
| `crab_sprite.h`, `crab_sprite.c` | generated crab frames (`firmware/tools/gen_crab.py`) |
| `sfx.h`, `sfx.c` | chimes on a worker task |
| `card_identity.h` | the built-in agent id and name (`firmware/tools/bind_identity.py`) |
| `key_guard.h` | upstream logic for swallowing the wake key after deep sleep; not used by the card, only by a host test |
| `ui_pixel*.c/h`, `demo*.c/h` | upstream demo code, not compiled into the card (see `main/CMakeLists.txt`) |

Other parts: `firmware/components/` (board support package and the DNS server), `firmware/assets/fonts/`
(the fonts, their licences and the generated font files), `firmware/tests/` (host tests).

## Generated assets

| Asset | Generator | Notes |
| --- | --- | --- |
| `assets/fonts/font_px12.c`, `font_px24.c` | `python firmware/tools/gen_fonts.py <path to lv_font_conv.js>` | Fusion Pixel 12 px (SIL OFL 1.1), 1 bpp, ASCII + all GB2312 hanzi + punctuation. Needs Node.js and `lv_font_conv` 1.5.3 |
| `font_mono11.c`, `font_mono12.c`, `font_mono12b.c`, `font_mono14.c` | same | Cascadia Mono (SIL OFL 1.1; the three static weights in `assets/fonts/` are the unchanged files of release v2407.24), 4 bpp, ASCII and the middle dot. They fall back to `font_px12`, so a Chinese name in a Latin line is still drawn |
| `font_big72.c`, `font_big60.c`, `font_big48.c`, `font_big32.c`, `font_big26.c`, `font_big21.c` | same | Cascadia Mono Bold, 4 bpp: only what a number, a level or a pairing code is made of (digits, a few signs, `K` `M` `L` `V`; capitals in the 72 and 48 px sizes). The three small sizes set what follows a number: its cents, its `K` or `M` |
| `main/crab_sprite.c` | `python firmware/tools/gen_crab.py` | 16x11 pixel art in the card's mint on its dark ground, two frames at 1x (the header's mark) and 10x (level-up poster), RGB565 with the ground baked in |

The colours in `gen_crab.py` repeat `C_MINT` and `C_BG` of `main/ui_card.c`; change them together. The generators
find the firmware folder from their own location, so they can be started from anywhere.

## Host tests

```bash
CJSON_DIR=<esp-idf>/components/json/cJSON firmware/tools/card_host_tests.sh [answers.jsonl ...]
```

It compiles `main/card_app.c`, `main/cloud.c` and `main/week.c` unchanged with a C compiler over a stand-in
platform (`tests/card_stubs`: time, LVGL timers, FreeRTOS queues, the HTTP client, NVS; the screen only records
what it was asked to draw). First it runs `tests/test_card_week.c`, the week of earnings on its own (the HTTP date,
midnight, days without the card, a total that falls and comes back, a restart, another agent, answers dated
oddly). Then three things:

1. checks of the behaviour described in this guide (pairing at the edge, pay decisions and retries, the holder's
   secret, separate tokens per server, USB commands that arrive together, closed connections and what is not
   resent, the week kept from the server's answers);
2. one script of USB messages and key presses through this `card_app.c` and through the 0.7.6 one (from git, or
   `tests/card_app_old.c`): both must print the same lines;
3. for each answers file given, a replay of a server's answers in order, printing what the screen showed; a file
   `X.screen` next to `X.jsonl` is compared.

The HTTP stand-in behaves like the ESP-IDF 5.5 client where the card can tell the difference (events, their order,
error codes, the status code left behind after a failure). The tests do not cover drawing (`main/ui_card.c`).
`tools/tests` at the repository root holds the tests of `tools/flash.py` (`python -m unittest discover -s tools/tests -v`).

## Known limitations

- **No money moves**, by design. Before it could: a signing device key on the card (so the server can verify that a
  confirmation came from this card, not only from its token), which depends on flash encryption and secure boot.
- **Flash encryption and secure boot are off.** Wi-Fi passwords and the device token are in plain text in flash and
  can be read over the cable. Enabling them burns eFuses and cannot be undone.
- **The USB commands are not authenticated** and cannot yet be compiled out.
- **The secret can be watched.** The screen shows only squares and USB echoes `*`, but someone beside the holder can
  see eight presses. While it is typed, the presses are in RAM and could be read over the USB debug port.
- **NFC proves nothing.** The NTAG213 content is fixed and can be copied; it only opens the page. Every confirmation
  must happen on the card.
- **No over-the-air update.** The partition table has one application partition; adding updates means changing the
  factory layout.
- **Power is not managed.** The screen stays on and the card polls every 4 s while paired. Battery life was not
  measured.
- **Keys act on press.** A hold of OK first delivers a press (on the resting face that makes the crab speak).
- **A proxy's unsolicited 408** before it closes an idle connection would be read as the answer to the next request
  (for a decision: "not paid"). It has not been seen on the site's path and is not handled.
- **Running out of memory would freeze the screen.** LVGL's handler for a failed allocation is its default, an
  endless loop, and the task watchdog only warns: the screen would stop until the card is restarted, while the USB
  link goes on answering. It has not been seen. With 0.11.0 the reference card has 69 kB of heap free at rest,
  level over ten minutes, and 61 kB at its lowest in that time (0.10: 72 and 65 kB); the lowest seen was 53 kB,
  while its screens were walked through and it connected to the site again.
- **A server can make the card restart.** ESP-IDF's HTTP client stops the program when it runs out of memory while
  taking in a response header (`http_utils_append_string`), and it takes headers longer than the card has memory
  for. The site does not send such headers; a server that did would restart the card on every answer.
- **The week is what the card saw.** It starts empty, has no figure for a day the card was off, and leaves out what
  was earned while the card was not asking; after the total was corrected downwards the bars stay empty until it
  has passed its earlier height. Its days begin at midnight of UTC+8, whatever the holder's time zone; there is no
  setting. It is made from the server's total and the server's time, and is as true as they are. The server has no
  route that gives the card a week of its own.
- **No clock and no battery level on the face.** The card learns the time only from the server's answers and does
  not read its battery.
- **Setup can wait for a request.** Choosing "connect Wi-Fi" while an HTTPS request (and its one resend) is under way
  delays the hotspot until it ends, which can take some tens of seconds; USB `api` and `unpair` take effect at the
  same point.
- **2.4 GHz only.** Phone hotspots work (iPhone: "Maximise Compatibility").
- On a reference unit with firmware 0.10 and earlier, the screens, pairing, pay requests with the secret, freezing
  and the task notices were exercised over Wi-Fi against a local test server and the card has run paired with the
  site. With 0.11.0 on the same unit (2026-11-25): every screen was walked through over USB with a picture of each
  (a computer playing the server's part), and the card runs paired with the site and reads its agent with its
  token; the Wi-Fi runs against a test server were not repeated, and the week has not seen an agent earn yet.
  Physical key feel, the look of the real panel (0.11 is the first version with smoothed type) and the speaker
  were not assessed. `docs/READINESS.md` has the detail.

## Version history

| Version | Change |
| --- | --- |
| 0.1 | Card face, offer with countdown, working, payout, level-up; data fed by a computer over USB; screenshots over USB |
| 0.2 | Built-in identity (`bind_identity.py`); USB gateway with the agent's public profile; local pay requests from a demo wallet; `pay`, `say` |
| 0.3 | Bell chimes and quiet hours; Wi-Fi over USB; the card fetches its public profile itself; screenshots streamed without a frame buffer |
| 0.4 | The card talks to the server: device-code pairing, device token, pay-request polling and decisions; USB `api` |
| 0.5 | Phone provisioning through the card's hotspot; up to four saved networks; settings menu (hold OK); stored sound switch; setup security review |
| 0.6 | Dark interface; approving takes a held OK |
| 0.7 | Spec-sheet layout and flat colours; USB test messages that play the server (`remote` pay, `pairing`, `setup_view`); "sending" no longer flashes black; plate number follows the agent; `job lost` and `job held` |
| 0.8 | The card polls its agent's tasks with a cursor: status line, new-task and payout sheets, queueing. 0.8.1: only 401 unpairs. 0.8.2: edge answers (403, 429, 5xx) no longer cost a pairing code or a decision; a dead code leaves the screen |
| 0.9 | The holder's secret (eight presses), boot id header, guard line on the face, separate tokens for the site and other servers. 0.9.1: `api` and `unpair` sent together delete the right token. 0.9.2: a request lost on a connection the server closed while idle is resent once at once |
| 0.10 | Brand colours: every screen stands on a dithered colour field; centred sheets. Behaviour unchanged |
| 0.11 | A new look: one dark ground with mint as the only accent, set from the left edge, Latin and digits in Cascadia Mono. The face is built around one number, the agent's lifetime earnings, with a one-word state in the header and a week of daily earnings as bars, which the card keeps itself from the server's `Date` header and the total. A long agent name is cut with an ellipsis. USB `stats` takes `week` and `wd`. What the keys do, the sheets' words and the server API are unchanged |
