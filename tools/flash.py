#!/usr/bin/env python3
"""Put the ClawHunt Agent Card firmware on a FoloToy AI Passport, and take it off again.

    python tools/flash.py plan <app image>          say what `flash` would do; no card is touched
    python tools/flash.py check                     look at the card; nothing is written (exit 1 when it cannot take the firmware)
    python tools/flash.py flash <app image>         back the card up, then write the application
    python tools/flash.py restore <backup.bin>      write the application of a backup back (the factory firmware, usually)

Only the application partition is ever written (0x10000, 3 MB). The bootloader, the partition table, the card's
own id partition (`cardid`), its stored Wi-Fi networks and its pairing with ClawHunt are in other partitions and
stay as they are. Writing a merged image from 0x0, as the upstream firmware's instructions do, erases `cardid`:
this tool has no command that writes anywhere else.

Before anything is written the tool makes sure of these, and stops if one does not hold:
  1. the image is a whole, undamaged ESP32-C3 application (its checksum and its own SHA-256 are recomputed) and
     it is the AI Passport's project (--any-image lifts the second half). The bytes that were checked are the
     bytes that are written: the file is not read a second time, and what follows the application in the file
     is not written. (`restore` writes the backup's application partition as the backup has it, all 3 MB.);
  2. the chip on the port is an ESP32-C3 with the AI Passport's 8 MB of flash;
  3. the card's partition table is the AI Passport's factory layout, entry for entry. A card with any other
     table is not written to: somebody has to look at it first;
  4. a backup of the whole flash has been read, the chip's own hash of its flash matches it, and it holds the
     partition table that was just read (--no-backup when you already have one);
  5. after you said yes: the card on the port is still the one that was looked at.
A card that was only looked at, or where something stopped, is restarted as it was.

It needs Python 3.9+ and esptool 5 (`pip install -r tools/requirements.txt`). Options: --port COM6 (found by
itself when exactly one Espressif USB port is present), --yes (do not ask), --backup-dir (default: a folder
`clawhunt-card-backups` in your home folder), --no-stub (talk to the chip's own loader, not the helper esptool
uploads: much slower; for when the helper does not run, which is the case in Espressif's emulator).

A backup holds everything on the card, its identity, its saved Wi-Fi passwords and its ClawHunt device token
among it. It is the only thing that can bring the card's identity back. Keep it, and keep it to yourself.
"""
from __future__ import annotations

import argparse
import functools
import hashlib
import json
import operator
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

FLASH_SIZE = 8 * 1024 * 1024
APP_OFFSET = 0x10000
APP_SIZE = 0x300000
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0xC00
ESP32C3_CHIP_ID = 5
IMAGE_MAGIC = 0xE9
IMAGE_HEADER_SIZE = 24
APP_DESC_OFFSET = 32            # the image header (24 bytes) and the first segment's header (8 bytes) come before it
APP_DESC_SIZE = 256
APP_DESC_MAGIC = 0xABCD5432
EXPECTED_PROJECT = "FoloToy-AI-Passport"   # the project name the firmware keeps from its upstream
ESPRESSIF_USB_VID = 0x303A
ENTRY = struct.Struct("<HBBII16sI")
ROOT = Path(__file__).resolve().parent.parent
KNOWN_IMAGES = ROOT / "tools" / "known_images.json"
ESPTOOL_OPTIONS: list = []      # what --no-stub adds to every esptool command of this run
_WORK: list = []                # this run's own folder: esptool is started in it and is given files from it
_REACHED: list = []             # not empty once esptool connected to the chip in this run


class Stop(Exception):
    """A check did not hold: nothing more is done."""


@dataclass(frozen=True)
class Partition:
    label: str
    kind: int       # 0 = app, 1 = data
    subtype: int
    offset: int
    size: int


@dataclass(frozen=True)
class Image:
    size: int       # the application itself: header, segments, checksum, its SHA-256
    file_size: int  # what is in the file; a partition cut out of a backup has more after the application
    sha256: str     # of the application itself
    chip_id: int
    project: str
    version: str
    built: str


@dataclass(frozen=True)
class Card:
    parts: tuple    # of Partition
    flash_size: int
    mac: str


# The partition table of a retail AI Passport (firmware/partitions.csv; tools/tests keeps the three in step).
# subtypes: 0x00 factory app, 0x01 phy, 0x02 nvs, 0x82 spiffs
FACTORY_LAYOUT = (
    Partition("nvs", 1, 0x02, 0x009000, 0x006000),
    Partition("phy_init", 1, 0x01, 0x00F000, 0x001000),
    Partition("factory", 0, 0x00, APP_OFFSET, APP_SIZE),
    Partition("imgstore", 1, 0x82, 0x310000, 0x020000),
    Partition("imgframe", 1, 0x82, 0x330000, 0x026000),
    Partition("cardid", 1, 0x02, 0x356000, 0x004000),
    Partition("imguser", 1, 0x82, 0x35A000, 0x020000),
    Partition("audio", 1, 0x82, 0x37A000, 0x080000),
    Partition("imgava", 1, 0x82, 0x3FA000, 0x100000),
)


# ---- what can be checked without a card -----------------------------------------------------------------------
def parse_partition_table(raw: bytes) -> list:
    """The entries of an ESP-IDF partition table as read from flash (0x8000, up to 0xC00 bytes).

    Erased flash gives no entries. A table with entries must end in its checksum entry, and the checksum must be
    that of the entries: a table that cannot prove itself stops the tool.
    """
    parts: list = []
    for i in range(0, len(raw) - 31, 32):
        entry = raw[i:i + 32]
        magic = struct.unpack_from("<H", entry)[0]
        if magic == 0x50AA:
            _, kind, subtype, offset, size, label, _flags = ENTRY.unpack(entry)
            parts.append(Partition(label.rstrip(b"\x00").decode("ascii", "replace"), kind, subtype, offset, size))
        elif magic == 0xEBEB:
            # not a use of MD5 for security: it is the checksum ESP-IDF puts there (a FIPS-mode Python must be told)
            if entry[2:16] != b"\xff" * 14 or hashlib.md5(raw[:i], usedforsecurity=False).digest() != entry[16:32]:
                raise Stop("the partition table's checksum does not match its entries: the read was bad, or this is not a partition table")
            return parts
        elif entry == b"\xff" * 32:
            break
        else:
            raise Stop(f"entry {i // 32} of the partition table is not a partition (magic 0x{magic:04x})")
    if parts:
        raise Stop("the partition table has no checksum entry: it cannot be trusted")
    return parts


def _says(p: Partition) -> str:
    return f"`{p.label}` ({'app' if p.kind == 0 else 'data'}, subtype 0x{p.subtype:02x}) at 0x{p.offset:x}, 0x{p.size:x} bytes"


def layout_problems(parts, file_size: int) -> list:
    """Why a card with this partition table must not be written with a file of that size; empty when it may."""
    if not parts:
        return [f"the card has no partition table where one is expected (0x{PARTITION_TABLE_OFFSET:x})"]
    problems = []
    apps = [p for p in parts if p.kind == 0]
    if not any(p.offset == APP_OFFSET for p in apps):
        where = ", ".join(f"{p.label} at 0x{p.offset:x}" for p in apps) or "none"
        problems.append(f"no application partition starts at 0x{APP_OFFSET:x} (application partitions on this card: {where})")
    if len(apps) > 1:
        problems.append("the card has more than one application partition (an OTA layout)")
    if not any(p.label == "cardid" for p in parts):
        problems.append("there is no `cardid` partition: this is not an AI Passport as shipped, or its flash was rewritten from 0x0 before")
    if not problems and tuple(parts) != FACTORY_LAYOUT:
        # the same partitions somewhere else, of another size, or with one more: a write could land on a neighbour
        found = tuple(parts)
        at = next((i for i, (a, b) in enumerate(zip(found, FACTORY_LAYOUT)) if a != b), min(len(found), len(FACTORY_LAYOUT)))
        have = _says(found[at]) if at < len(found) else "nothing"
        want = _says(FACTORY_LAYOUT[at]) if at < len(FACTORY_LAYOUT) else "nothing"
        problems.append(f"entry {at} of the partition table is {have}; an AI Passport has {want} there")
    if problems:
        problems[0] = "the partition table is not the AI Passport's factory layout: " + problems[0]
    if file_size > APP_SIZE:
        problems.append(f"the image ({file_size} bytes) is larger than the application partition ({APP_SIZE} bytes)")
    return problems


def app_description(raw: bytes) -> Optional[tuple]:
    """Project, version and build time from the start of an application image; None when there is no description."""
    if len(raw) < APP_DESC_OFFSET + APP_DESC_SIZE or struct.unpack_from("<I", raw, APP_DESC_OFFSET)[0] != APP_DESC_MAGIC:
        return None

    def text(a: int, b: int) -> str:
        return raw[APP_DESC_OFFSET + a:APP_DESC_OFFSET + b].split(b"\x00")[0].decode("ascii", "replace")

    return text(48, 80), text(16, 48), f"{text(96, 112)} {text(80, 96)}"


def image_of(raw: bytes, name: str) -> Image:
    """What an application image says about itself; stops when it is not a whole, undamaged ESP32-C3 application."""
    if len(raw) < APP_DESC_OFFSET + APP_DESC_SIZE or raw[0] != IMAGE_MAGIC:
        raise Stop(f"{name} is not an ESP application image (it does not start with 0x{IMAGE_MAGIC:02x})")
    chip_id = struct.unpack_from("<H", raw, 12)[0]
    if chip_id != ESP32C3_CHIP_ID:
        raise Stop(f"{name} is built for chip id {chip_id}, not for the ESP32-C3 ({ESP32C3_CHIP_ID})")
    described = app_description(raw)
    if described is None or struct.unpack_from("<I", raw, IMAGE_HEADER_SIZE + 4)[0] < APP_DESC_SIZE:
        raise Stop(f"{name} has no application description where one is expected: is it a bootloader, or a merged image from 0x0?")
    if not 1 <= raw[1] <= 16:
        raise Stop(f"{name} says it has {raw[1]} segments: not an application image")
    if raw[23] != 1:
        raise Stop(f"{name} does not carry its own SHA-256, as every ESP-IDF build does: it cannot be checked for damage")
    # the segments, then padding up to a checksum byte that ends a 16-byte block, then the SHA-256 of all before it
    pos = IMAGE_HEADER_SIZE
    checksum = 0xEF
    for _ in range(raw[1]):
        if pos + 8 > len(raw):
            raise Stop(f"{name} is cut short (a segment's header is missing)")
        size = struct.unpack_from("<I", raw, pos + 4)[0]
        pos += 8
        if pos + size > len(raw):
            raise Stop(f"{name} is cut short (a segment says {size} bytes, the file ends before that)")
        checksum = functools.reduce(operator.xor, raw[pos:pos + size], checksum)
        pos += size
    end = (pos // 16 + 1) * 16
    if end + 32 > len(raw):
        raise Stop(f"{name} is cut short (its checksum or its SHA-256 is missing)")
    if raw[end - 1] != checksum:
        raise Stop(f"{name} is damaged: its checksum is 0x{raw[end - 1]:02x}, its segments add up to 0x{checksum:02x}")
    if hashlib.sha256(raw[:end]).digest() != raw[end:end + 32]:
        raise Stop(f"{name} is damaged: the SHA-256 at its end is not the SHA-256 of what is before it")
    end += 32
    project, version, built = described
    return Image(size=end, file_size=len(raw), sha256=hashlib.sha256(raw[:end]).hexdigest(), chip_id=chip_id,
                 project=project, version=version, built=built)


def read_image(path: Path) -> Image:
    return image_of(path.read_bytes(), path.name)


def known(image: Image) -> Optional[str]:
    """What the repository says about an image with this checksum, if it lists it."""
    if not KNOWN_IMAGES.exists():
        return None
    try:
        listed = json.loads(KNOWN_IMAGES.read_text(encoding="utf-8"))["images"]
        return next((str(item["what"]) for item in listed if item["sha256"] == image.sha256), None)
    except (ValueError, LookupError, TypeError) as bad:
        raise Stop(f"{KNOWN_IMAGES.name} is not the list of images it should be ({type(bad).__name__}: {bad})") from bad


def describe(image: Image, path) -> list:
    lines = [f"image: {path}", f"  {image.size} bytes, sha256 {image.sha256}",
             f"  project `{image.project}`, version `{image.version}`, built {image.built}",
             "  whole and undamaged (checksum and SHA-256 recomputed)"]
    if image.file_size != image.size:
        lines.append(f"  the file holds {image.file_size - image.size} more bytes after the application (a partition read back from a card "
                     "does): `flash` does not write them")
    what = known(image)
    lines.append(f"  listed in tools/known_images.json: {what}" if what else
                 "  not listed in tools/known_images.json: an image you built yourself, or one nobody recorded")
    return lines


def must_be_the_cards(image: Image, name: str, any_image: bool, expect_version: Optional[str] = None) -> None:
    if image.project != EXPECTED_PROJECT and not any_image:
        raise Stop(f"{name} is an ESP32-C3 application, but of the project `{image.project}`, not `{EXPECTED_PROJECT}`: "
                   "it is not this card's firmware. --any-image flashes it anyway.")
    if expect_version is not None and image.version != expect_version:
        raise Stop(f"{name} says it is version `{image.version}`, not `{expect_version}`")


# ---- the card -------------------------------------------------------------------------------------------------
def workdir() -> Path:
    """This run's own empty folder. esptool is started in it (python puts the folder it is started in on its module
    path: a stray esptool.py beside the user must not be the one that runs) and gets its files from it."""
    if not _WORK:
        _WORK.append(tempfile.TemporaryDirectory(prefix="card-flash-"))
    return Path(_WORK[0].name)


def find_port(asked: Optional[str]) -> str:
    if asked:
        # it goes on esptool's command line, where a leading - is an option and a leading @ a file of arguments
        if not re.fullmatch(r"[A-Za-z0-9/][A-Za-z0-9_./:-]*", asked):
            raise Stop(f"`{asked}` is not a port name this tool passes on (COM6, /dev/ttyACM0, socket://127.0.0.1:5555)")
        return asked
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise Stop("pyserial is not installed: pip install -r tools/requirements.txt") from exc
    ports = [p for p in list_ports.comports() if p.vid == ESPRESSIF_USB_VID]
    if len(ports) == 1:
        return ports[0].device
    if not ports:
        raise Stop("no Espressif USB port found. Plug the card in with a data cable and wake it with a key, or name the port: --port COM6")
    raise Stop("more than one Espressif USB port: " + ", ".join(p.device for p in ports) + ". Name the card's: --port ...")


def esptool(port: str, *args: str, after: str = "no-reset") -> str:
    """Run esptool against the card, showing what it prints; returns that text. Stops when esptool fails."""
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c3", "--port", port, "--after", after, *ESPTOOL_OPTIONS, *args]
    print("  $ esptool " + " ".join(cmd[3:]))
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=str(workdir()),
                            text=True, encoding="utf-8", errors="replace")
    lines = []
    try:
        for line in proc.stdout:
            lines.append(line)
            if line.strip():
                print("    " + line.rstrip())
        code = proc.wait()
    except BaseException:
        proc.kill()         # an interrupt must not leave esptool holding the port
        proc.wait()
        raise
    finally:
        proc.stdout.close()
    out = "".join(lines)
    if "Connected to" in out:
        _REACHED.append(port)   # the chip answered: it sits in its loader now, whether or not the command went well
    if code != 0:
        if "No module named esptool" in out:
            raise Stop("esptool is not installed: pip install -r tools/requirements.txt")
        raise Stop(f"esptool failed (exit {code}); what it said is above")
    return out


def _field(text: str, label: str) -> Optional[str]:
    """The value esptool printed after `label:` on a line of its own."""
    for line in text.splitlines():
        if line.strip().lower().startswith(label.lower() + ":"):
            return line.split(":", 1)[1].strip()
    return None


def chip_of(text: str) -> str:
    """The chip esptool connected to (its line "Chip type:          ESP32-C3 (QFN32) (revision v0.4)")."""
    return _field(text, "Chip type") or ""


def flash_size_of(text: str) -> int:
    """The flash size esptool detected, in bytes (its `flash-id` output: "Detected flash size: 8MB")."""
    value = _field(text, "Detected flash size")
    if value is None:
        raise Stop("esptool did not say how large the card's flash is")
    if value.endswith("MB") and value[:-2].isdigit():
        return int(value[:-2]) * 1024 * 1024
    raise Stop(f"esptool says the card's flash is `{value}`: not a size this tool knows")


def mac_of(text: str) -> str:
    """The card's MAC as twelve hex digits; "unknown" when esptool printed none that reads as one."""
    value = (_field(text, "MAC") or "").replace(":", "").lower()
    return value if re.fullmatch(r"[0-9a-f]{12}", value) else "unknown"


def look(port: str) -> Card:
    """The card's partitions, the size of its flash and its MAC. Reads only."""
    info = esptool(port, "flash-id")
    if not chip_of(info).upper().startswith("ESP32-C3"):
        raise Stop(f"the chip on this port is `{chip_of(info) or 'not named by esptool'}`, not an ESP32-C3")
    out = workdir() / "partitions.bin"
    esptool(port, "read-flash", hex(PARTITION_TABLE_OFFSET), hex(PARTITION_TABLE_SIZE), str(out))
    return Card(tuple(parse_partition_table(out.read_bytes())), flash_size_of(info), mac_of(info))


def card_problems(found: Card, file_size: int) -> list:
    """Why this card must not be written with a file of that size; an empty list when it may."""
    problems = []
    if found.flash_size != FLASH_SIZE:
        problems.append(f"its flash is {found.flash_size // (1024 * 1024)} MB; an AI Passport has {FLASH_SIZE // (1024 * 1024)} MB")
    if found.mac == "unknown":
        problems.append("esptool did not say the card's MAC: the card cannot be told from another one")
    return problems + layout_problems(found.parts, file_size)


def on_card(port: str) -> Optional[tuple]:
    """Project, version and build time of the application the card holds now; None when there is none to read."""
    out = workdir() / "head.bin"
    esptool(port, "read-flash", hex(APP_OFFSET), hex(APP_DESC_OFFSET + APP_DESC_SIZE), str(out))
    head = out.read_bytes()
    return app_description(head) if head[:1] == bytes([IMAGE_MAGIC]) else None


def backup(port: str, found: Card, folder: Path) -> Path:
    """Read the whole flash into the folder and make sure the file is the flash. Returns the file."""
    folder = folder.resolve()
    if not folder.exists():
        folder.mkdir(parents=True, mode=0o700)      # what is in it is for its owner only
    stamp = time.strftime("%Y%m%d-%H%M%S")
    final = folder / f"card-{found.mac}-{stamp}.bin"
    count = 1
    while final.exists() or (folder / (final.name + ".partial")).exists():     # a backup is never written over
        count += 1
        final = folder / f"card-{found.mac}-{stamp}-{count}.bin"
    partial = folder / (final.name + ".partial")       # a read that stopped half way must not look like a backup
    print(f"reading the whole flash ({FLASH_SIZE // (1024 * 1024)} MB) into {final} (a minute or two)")
    # made here, for its owner only, before esptool fills it: it holds the card's secrets from its first byte on
    os.close(os.open(partial, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600))
    try:
        esptool(port, "read-flash", "0", hex(FLASH_SIZE), str(partial))
        raw = partial.read_bytes()
        if len(raw) != FLASH_SIZE:
            raise Stop(f"the backup is {len(raw)} bytes, the flash is {FLASH_SIZE}: the card was not written to")
        # the chip hashes its own flash and esptool compares that with the file: a read that went wrong shows here
        esptool(port, "verify-flash", "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep", "0", str(partial))
        table = raw[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE]
        if tuple(parse_partition_table(table)) != found.parts:
            raise Stop("the backup does not hold the partition table that was just read from the card: the card was not written to")
        os.replace(partial, final)
    except BaseException:
        try:
            partial.unlink()
        except OSError:
            pass
        raise
    print(f"  backup sha256 {hashlib.sha256(raw).hexdigest()}")
    try:
        held = image_of(raw[APP_OFFSET:APP_OFFSET + APP_SIZE], "the application on the card")
        print(f"  it holds the application that was on the card: project `{held.project}`, version `{held.version}`, built {held.built}")
    except Stop as broken:
        print(f"  NOTE: {broken}. This backup still brings back the card's identity and settings, but `restore` has no working "
              "application to take from it: keep the backup you made before.")
    return final


@contextmanager
def card(port: str):
    """The card sits in its loader while it is read. Leaving this block restarts it, unless the last command did."""
    state = {"restart": True}
    try:
        yield state
    finally:
        # a card that never answered was not taken out of what it was doing: there is nothing to restart
        if state["restart"] and _REACHED:
            try:
                esptool(port, "read-mac", after="hard-reset")
            except Stop:
                print("could not restart the card: unplug it and plug it in again", file=sys.stderr)


def ask(question: str, yes: bool) -> None:
    if yes:
        return
    try:
        answer = input(f"{question} Type yes to go on: ")
    except EOFError:
        answer = ""
    if answer.strip().lower() != "yes":
        raise Stop("you did not say yes: nothing was written")


def show_card(found: Card) -> None:
    print(f"ESP32-C3, MAC {found.mac}, flash {found.flash_size // (1024 * 1024)} MB; partitions:")
    for p in found.parts:
        print(f"  {p.label:9s} {'app ' if p.kind == 0 else 'data'} 0x{p.offset:06x} +0x{p.size:06x}")


def write_application(port: str, found: Card, data: bytes, state: dict, undo: str) -> None:
    """Write these bytes to the application partition of the card that was looked at, and to nothing else."""
    if look(port) != found:
        raise Stop("the card on the port is not the card that was checked a moment ago: nothing was written")
    staged = workdir() / "application.bin"      # the bytes that were checked, not whatever the file holds by now
    staged.write_bytes(data)
    try:
        esptool(port, "write-flash", hex(APP_OFFSET), str(staged), after="hard-reset")
    except BaseException:
        print("THE WRITE DID NOT FINISH. The application partition may be incomplete and the card may not start. Nothing "
              f"outside it was written. Run the same command again, or {undo}.", file=sys.stderr)
        raise
    state["restart"] = False


# ---- commands -------------------------------------------------------------------------------------------------
def cmd_plan(args) -> None:
    path = Path(args.image)
    image = read_image(path)
    print("\n".join(describe(image, path)))
    must_be_the_cards(image, path.name, args.any_image, args.expect_version)
    print("what `flash` would do with it:")
    print("  1. ask the chip what it is and how large its flash is (stops unless it is an ESP32-C3 with 8 MB)")
    print(f"  2. read the partition table at 0x{PARTITION_TABLE_OFFSET:x} (stops unless it is the AI Passport's factory layout, entry for entry)")
    print("  3. read the whole flash into the backup folder and have the chip confirm the copy (unless --no-backup)")
    print("  4. ask you")
    print("  5. look at the card once more (stops if it is another card now)")
    print(f"  6. write the {image.size} bytes that were checked to 0x{APP_OFFSET:x}; the chip hashes what it wrote and esptool compares")
    print("  7. restart the card")
    print("nothing else is written: bootloader, partition table, cardid, saved Wi-Fi and the pairing stay.")


def cmd_check(args) -> None:
    port = find_port(args.port)
    print(f"card on {port}")
    with card(port):
        found = look(port)
        show_card(found)
        problems = card_problems(found, 0)
        if problems:        # exit 1, so that a script can tell
            raise Stop("this card cannot take the firmware as it is:\n  - " + "\n  - ".join(problems))
        print("the layout is the AI Passport's: the firmware can be written to it")
        running = on_card(port)
        print("on the card now: " + (f"project `{running[0]}`, version `{running[1]}`, built {running[2]}" if running else "no application image"))


def cmd_flash(args) -> None:
    path = Path(args.image)
    raw = path.read_bytes()             # read once: these bytes are checked, and these bytes are written
    image = image_of(raw, path.name)
    print("\n".join(describe(image, path)))
    must_be_the_cards(image, path.name, args.any_image, args.expect_version)
    application = raw[:image.size]      # what was checked, and nothing that follows it in the file
    port = find_port(args.port)
    print(f"card on {port}")
    with card(port) as state:
        found = look(port)
        show_card(found)
        problems = card_problems(found, len(application))
        if problems:
            raise Stop("not flashing:\n  - " + "\n  - ".join(problems))
        undo = "write a backup of this card back with `restore`"
        if args.no_backup:
            print("no backup taken (--no-backup): make sure you have one of this card")
        else:
            kept = backup(port, found, Path(args.backup_dir))
            undo = f"put back what was on the card: python tools/flash.py restore \"{kept}\""
            print(f"to {undo}")
        ask(f"Write {path.name} to 0x{APP_OFFSET:x} of the card with MAC {found.mac}?", args.yes)
        write_application(port, found, application, state, undo)
    print("written and verified; the card restarts. Its pairing, Wi-Fi and identity were not touched.")


def cmd_restore(args) -> None:
    source = Path(args.backup)
    full = source.read_bytes()
    if len(full) != FLASH_SIZE:
        raise Stop(f"{source.name} is {len(full)} bytes; a whole-flash backup of an AI Passport is {FLASH_SIZE}")
    table = parse_partition_table(full[PARTITION_TABLE_OFFSET:PARTITION_TABLE_OFFSET + PARTITION_TABLE_SIZE])
    problems = layout_problems(table, 0)
    if problems:
        raise Stop(f"{source.name} is not a backup of an AI Passport:\n  - " + "\n  - ".join(problems))
    piece = full[APP_OFFSET:APP_OFFSET + APP_SIZE]
    image = image_of(piece, "the backup's application")
    print(f"the backup's application: project `{image.project}`, version `{image.version}`, built {image.built}; whole and undamaged")
    what = known(image)
    if what:
        print(f"  listed in tools/known_images.json: {what}")
    port = find_port(args.port)
    print(f"card on {port}")
    with card(port) as state:
        found = look(port)
        show_card(found)
        problems = card_problems(found, len(piece))
        if problems:
            raise Stop("not restoring:\n  - " + "\n  - ".join(problems))
        named = re.match(r"card-([0-9a-f]{12})-", source.name)
        if named and named.group(1) != found.mac:
            print(f"NOTE: by its name this backup is of the card {named.group(1)}; this card is {found.mac}. Only the application "
                  "is taken from it, and an application is not particular to a card.")
        ask(f"Write the backup's application back to 0x{APP_OFFSET:x} of the card with MAC {found.mac}?", args.yes)
        write_application(port, found, piece, state, "run it again")
    print("the backup's application is back on the card; nothing else was written.")


def main(argv: Optional[list] = None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            # esptool's lines may hold characters the console cannot show; and when the output goes to a file, a
            # line must be written when it is printed, or what is said on stderr lands in the middle of older lines
            stream.reconfigure(errors="replace", line_buffering=True)
    parser = argparse.ArgumentParser(description="Flash the ClawHunt Agent Card firmware onto a FoloToy AI Passport (application partition only).")
    sub = parser.add_subparsers(dest="command", required=True)
    for name, func, needs in (("plan", cmd_plan, "image"), ("check", cmd_check, None), ("flash", cmd_flash, "image"), ("restore", cmd_restore, "backup")):
        p = sub.add_parser(name)
        if needs:
            p.add_argument(needs)
        if name != "plan":
            p.add_argument("--port", help="the card's serial port; found by itself when there is exactly one Espressif USB port")
            p.add_argument("--no-stub", action="store_true",
                           help="talk to the chip's own loader instead of the helper esptool uploads: much slower, for when the helper does not run")
        if name in ("plan", "flash"):
            p.add_argument("--any-image", action="store_true", help=f"accept an ESP32-C3 application that is not the `{EXPECTED_PROJECT}` project")
            p.add_argument("--expect-version", help="stop unless the image says it is this version")
        if name in ("flash", "restore"):
            p.add_argument("--yes", action="store_true", help="do not ask before writing")
        if name == "flash":
            p.add_argument("--no-backup", action="store_true", help="do not read the whole flash first")
            p.add_argument("--backup-dir", default=str(Path.home() / "clawhunt-card-backups"),
                           help="where the whole-flash backup goes (default: clawhunt-card-backups in your home folder)")
        p.set_defaults(func=func)
    args = parser.parse_args(argv)
    ESPTOOL_OPTIONS[:] = ["--no-stub"] if getattr(args, "no_stub", False) else []
    try:
        args.func(args)
    except Stop as stop:
        print(f"STOPPED: {stop}", file=sys.stderr)
        return 1
    except FileNotFoundError as missing:
        print(f"STOPPED: no such file: {missing.filename}", file=sys.stderr)
        return 1
    except OSError as failed:
        print(f"STOPPED: {failed.strerror or failed}" + (f": {failed.filename}" if failed.filename else ""), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("STOPPED: interrupted", file=sys.stderr)
        return 1
    finally:
        while _WORK:
            try:
                _WORK.pop().cleanup()
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
