"""Tests of tools/flash.py. No card is needed and none is touched.

    python -m unittest discover -s tools/tests -v

Three kinds:
- the parts that read bytes (a partition table, an application image, esptool's lines), against the partition
  table ESP-IDF builds from firmware/partitions.csv (fixtures/partition-table.hex, byte for byte a retail card's)
  and against images made here to the ESP image format;
- the whole command line, against a "card" that is a file (fake_esptool/): what is read, what is written, in which
  order, that every byte outside the application partition is as it was, and what happens when something goes
  wrong on the way (a read that comes back wrong, another card on the port, a write that stops half way, the
  image file changing under the tool);
- real files, when CARD_REAL_FILES names a folder that has them (a whole-flash backup `stock-*.bin`, application
  images `app-*.bin`): they stay on the machine, so these are skipped anywhere else.
"""
import hashlib
import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import time
import types
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))

import flash  # noqa: E402

APP_OFFSET = 0x10000
APP_SIZE = 0x300000
FLASH_SIZE = 8 * 1024 * 1024
PORT = "NO-SUCH-PORT"     # not a device: were the real esptool ever run by these tests, it could open nothing
TABLE_AT = slice(flash.PARTITION_TABLE_OFFSET, flash.PARTITION_TABLE_OFFSET + flash.PARTITION_TABLE_SIZE)


def fixture_table() -> bytes:
    rows = [line for line in (HERE / "fixtures" / "partition-table.hex").read_text(encoding="ascii").splitlines() if line and not line.startswith("#")]
    raw = b"".join(bytes.fromhex(row) for row in rows)
    return raw + b"\xff" * (flash.PARTITION_TABLE_SIZE - len(raw))


def make_table(entries, checksum=True) -> bytes:
    """A partition table of (label, kind, subtype, offset, size) entries, with its checksum entry."""
    body = b"".join(flash.ENTRY.pack(0x50AA, kind, subtype, offset, size, label.encode("ascii"), 0) for label, kind, subtype, offset, size in entries)
    raw = body + (b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(body, usedforsecurity=False).digest() if checksum else b"")
    return raw + b"\xff" * (flash.PARTITION_TABLE_SIZE - len(raw))


def entries_of(table: bytes):
    return [(p.label, p.kind, p.subtype, p.offset, p.size) for p in flash.parse_partition_table(table)]


def factory_entries():
    return [(p.label, p.kind, p.subtype, p.offset, p.size) for p in flash.FACTORY_LAYOUT]


def make_image(project="FoloToy-AI-Passport", version="0.10.0", chip_id=5, payload=b"card" * 4096, hashed=True, segments=1) -> bytes:
    """An application image in the ESP image format: header, one segment that starts with the description, checksum, SHA-256."""
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0, flash.APP_DESC_MAGIC)
    for text, at in ((version, 16), (project, 48), ("12:41:06", 80), ("Nov 18 2026", 96), ("v5.5.3", 112)):
        desc[at:at + len(text)] = text.encode("ascii")
    segment = bytes(desc) + payload
    header = bytearray(24)
    header[0] = flash.IMAGE_MAGIC
    header[1] = segments
    struct.pack_into("<H", header, 12, chip_id)
    header[23] = 1 if hashed else 0
    body = bytes(header) + struct.pack("<II", 0x3C000020, len(segment)) + segment
    checksum = 0xEF
    for byte in segment:
        checksum ^= byte
    body += b"\x00" * (15 - len(body) % 16) + bytes([checksum])
    return body + (hashlib.sha256(body).digest() if hashed else b"")


class PartitionTable(unittest.TestCase):
    def test_the_csv_the_fixture_and_the_tools_own_table_are_one(self):
        # firmware/partitions.csv is what the firmware is built with, the fixture is what ESP-IDF makes of it (and what
        # a retail card holds), FACTORY_LAYOUT is what the tool insists on: the three must say the same
        kinds = {"app": 0, "data": 1}
        subtypes = {"factory": 0x00, "phy": 0x01, "nvs": 0x02, "spiffs": 0x82}
        from_csv = []
        for line in (ROOT / "firmware" / "partitions.csv").read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            name, kind, subtype, offset, size = [field.strip() for field in line.split(",")[:5]]
            from_csv.append((name, kinds[kind], subtypes[subtype], int(offset, 0), int(size, 0)))
        self.assertEqual(entries_of(fixture_table()), from_csv)
        self.assertEqual(factory_entries(), from_csv)
        factory = next(p for p in flash.FACTORY_LAYOUT if p.kind == 0)
        self.assertEqual((factory.offset, factory.size), (flash.APP_OFFSET, flash.APP_SIZE))
        self.assertLessEqual(max(p.offset + p.size for p in flash.FACTORY_LAYOUT), flash.FLASH_SIZE)

    def test_the_cards_layout_takes_the_firmware(self):
        parts = flash.parse_partition_table(fixture_table())
        self.assertEqual(flash.layout_problems(parts, 2_472_464), [])
        self.assertEqual(flash.layout_problems(parts, APP_SIZE), [])

    def test_an_image_larger_than_the_partition_is_refused(self):
        parts = flash.parse_partition_table(fixture_table())
        problems = flash.layout_problems(parts, APP_SIZE + 1)
        self.assertEqual(len(problems), 1)
        self.assertIn("larger than the application partition", problems[0])

    def test_a_card_without_cardid_is_refused(self):
        table = make_table([e for e in factory_entries() if e[0] != "cardid"])
        problems = flash.layout_problems(flash.parse_partition_table(table), 1000)
        self.assertEqual(len(problems), 1)
        self.assertIn("cardid", problems[0])

    def test_an_application_somewhere_else_is_refused(self):
        table = make_table([("nvs", 1, 2, 0x9000, 0x6000), ("factory", 0, 0, 0x20000, 0x300000), ("cardid", 1, 2, 0x356000, 0x4000)])
        self.assertIn("no application partition starts at 0x10000", flash.layout_problems(flash.parse_partition_table(table), 1000)[0])

    def test_an_ota_layout_is_refused(self):
        table = make_table([("factory", 0, 0, 0x10000, 0x100000), ("ota_0", 0, 0x10, 0x110000, 0x100000), ("cardid", 1, 2, 0x356000, 0x4000)])
        self.assertTrue(any("more than one application partition" in p for p in flash.layout_problems(flash.parse_partition_table(table), 1000)))

    def test_an_application_partition_that_reaches_over_cardid_is_refused(self):
        # every partition a card needs is there and the image would fit "the application partition": but that
        # partition is 4 MB here and ends past cardid
        entries = [(label, kind, subtype, offset, 0x400000 if label == "factory" else size) for label, kind, subtype, offset, size in factory_entries()]
        problems = flash.layout_problems(flash.parse_partition_table(make_table(entries)), 0x380000)
        self.assertTrue(problems)
        self.assertIn("not the AI Passport's factory layout", problems[0])
        self.assertIn("entry 2", problems[0])

    def test_any_other_difference_from_the_factory_layout_is_refused(self):
        changed = {
            "one partition more": factory_entries() + [("extra", 1, 0x82, 0x4FA000, 0x1000)],
            "one partition fewer": factory_entries()[:-1],
            "cardid somewhere else": [(l, k, s, 0x357000 if l == "cardid" else o, z) for l, k, s, o, z in factory_entries()],
            "another subtype": [(l, k, 0x81 if l == "imgava" else s, o, z) for l, k, s, o, z in factory_entries()],
            "another name": [("imgava2" if l == "imgava" else l, k, s, o, z) for l, k, s, o, z in factory_entries()],
        }
        for what, entries in changed.items():
            problems = flash.layout_problems(flash.parse_partition_table(make_table(entries)), 1000)
            self.assertEqual(len(problems), 1, what)
            self.assertIn("not the AI Passport's factory layout", problems[0], what)

    def test_erased_flash_has_no_table(self):
        self.assertEqual(flash.parse_partition_table(b"\xff" * flash.PARTITION_TABLE_SIZE), [])
        self.assertIn("no partition table", flash.layout_problems([], 1000)[0])

    def test_a_bad_checksum_stops(self):
        table = bytearray(fixture_table())
        table[5] ^= 0x01                           # one bit of the first entry's offset
        with self.assertRaisesRegex(flash.Stop, "checksum does not match"):
            flash.parse_partition_table(bytes(table))

    def test_a_table_without_its_checksum_entry_stops(self):
        with self.assertRaisesRegex(flash.Stop, "no checksum entry"):
            flash.parse_partition_table(make_table(factory_entries(), checksum=False))
        full = b"".join(flash.ENTRY.pack(0x50AA, 1, 2, 0x9000 + i * 0x1000, 0x1000, b"p%d" % i, 0) for i in range(96))
        with self.assertRaisesRegex(flash.Stop, "no checksum entry"):
            flash.parse_partition_table(full)

    def test_a_checksum_entry_that_is_not_one_stops(self):
        table = bytearray(fixture_table())
        at = len(factory_entries()) * 32
        self.assertEqual(table[at:at + 2], b"\xeb\xeb")
        table[at + 7] = 0x00                       # the filler between the mark and the checksum
        with self.assertRaisesRegex(flash.Stop, "checksum does not match"):
            flash.parse_partition_table(bytes(table))

    def test_something_that_is_not_a_table_stops(self):
        with self.assertRaisesRegex(flash.Stop, "is not a partition"):
            flash.parse_partition_table(b"\x00" * flash.PARTITION_TABLE_SIZE)


class ApplicationImage(unittest.TestCase):
    def test_a_whole_image_is_read(self):
        raw = make_image()
        image = flash.image_of(raw, "x.bin")
        self.assertEqual((image.project, image.version, image.built, image.chip_id), ("FoloToy-AI-Passport", "0.10.0", "Nov 18 2026 12:41:06", 5))
        self.assertEqual((image.size, image.file_size), (len(raw), len(raw)))
        self.assertEqual(image.sha256, hashlib.sha256(raw).hexdigest())

    def test_what_follows_the_image_in_a_partition_is_not_part_of_it(self):
        raw = make_image()
        image = flash.image_of(raw + b"\xff" * 5000, "partition")
        self.assertEqual((image.size, image.file_size), (len(raw), len(raw) + 5000))
        self.assertEqual(image.sha256, hashlib.sha256(raw).hexdigest())

    def test_an_image_without_its_own_hash_is_refused(self):
        # the 8-bit checksum alone would let through most damage that changes two bytes
        with self.assertRaisesRegex(flash.Stop, "does not carry its own SHA-256"):
            flash.image_of(make_image(hashed=False) + b"\xff" * 64, "x.bin")

    def test_one_changed_bit_stops(self):
        raw = make_image()
        for at in (300, len(raw) // 2, len(raw) - 40, len(raw) - 1):
            broken = bytearray(raw)
            broken[at] ^= 0x01
            with self.assertRaisesRegex(flash.Stop, "damaged"):
                flash.image_of(bytes(broken), "x.bin")

    def test_two_changed_bits_that_cancel_in_the_checksum_are_caught_by_the_hash(self):
        broken = bytearray(make_image())
        broken[1000] ^= 0x01
        broken[1001] ^= 0x01
        with self.assertRaisesRegex(flash.Stop, "SHA-256 at its end"):
            flash.image_of(bytes(broken), "x.bin")

    def test_a_cut_file_stops(self):
        raw = make_image()
        for keep in (len(raw) - 1, len(raw) - 33, len(raw) // 2, 300):
            with self.assertRaisesRegex(flash.Stop, "cut short"):
                flash.image_of(raw[:keep], "x.bin")

    def test_another_chips_image_stops(self):
        with self.assertRaisesRegex(flash.Stop, "not for the ESP32-C3"):
            flash.image_of(make_image(chip_id=9), "x.bin")

    def test_not_an_image_stops(self):
        with self.assertRaisesRegex(flash.Stop, "not an ESP application image"):
            flash.image_of(b"\x00" * 4096, "x.bin")
        with self.assertRaisesRegex(flash.Stop, "not an ESP application image"):
            flash.image_of(b"\xe9", "x.bin")

    def test_an_image_without_a_description_stops(self):
        raw = bytearray(make_image())
        struct.pack_into("<I", raw, flash.APP_DESC_OFFSET, 0)
        with self.assertRaisesRegex(flash.Stop, "no application description"):
            flash.image_of(bytes(raw), "bootloader.bin")

    def test_a_description_that_is_not_inside_the_first_segment_stops(self):
        raw = bytearray(make_image())
        struct.pack_into("<I", raw, flash.IMAGE_HEADER_SIZE + 4, 100)      # the first segment says it is 100 bytes
        with self.assertRaisesRegex(flash.Stop, "no application description"):
            flash.image_of(bytes(raw), "x.bin")

    def test_a_segment_count_that_cannot_be_stops(self):
        for count in (0, 17, 255):
            with self.assertRaisesRegex(flash.Stop, "segments"):
                flash.image_of(make_image(segments=count), "x.bin")

    def test_another_projects_image_needs_the_flag(self):
        image = flash.image_of(make_image(project="blink"), "blink.bin")
        with self.assertRaisesRegex(flash.Stop, "not this card's firmware"):
            flash.must_be_the_cards(image, "blink.bin", any_image=False)
        flash.must_be_the_cards(image, "blink.bin", any_image=True)
        flash.must_be_the_cards(flash.image_of(make_image(), "x.bin"), "x.bin", any_image=False)

    def test_the_version_can_be_insisted_on(self):
        image = flash.image_of(make_image(version="0.10.0"), "x.bin")
        flash.must_be_the_cards(image, "x.bin", any_image=False, expect_version="0.10.0")
        with self.assertRaisesRegex(flash.Stop, "not `0.10.1`"):
            flash.must_be_the_cards(image, "x.bin", any_image=False, expect_version="0.10.1")


class EsptoolLines(unittest.TestCase):
    # what esptool 5.6.0 printed for `flash-id` (the chip was Espressif's emulator; a card's MAC is not all zero)
    REAL = """esptool v5.6.0
Serial port socket://127.0.0.1:5555:
Connecting...
Connected to ESP32-C3 on socket://127.0.0.1:5555:
Chip type:          ESP32-C3 (QFN32) (revision v0.3)
Features:           Wi-Fi, BT 5 (LE), Single Core, 160MHz
Crystal frequency:  40MHz
MAC:                00:00:00:00:00:00

Uploading stub flasher...
Running stub flasher...
Stub flasher running.

Flash Memory Information:
=========================
Manufacturer: c8
Device: 4017
Detected flash size: 8MB

Staying in bootloader.
"""

    def test_flash_size(self):
        self.assertEqual(flash.flash_size_of(self.REAL), FLASH_SIZE)
        self.assertEqual(flash.flash_size_of("Features: Embedded Flash 4MB (XMC)\nDetected flash size: 16MB\n"), 16 * 1024 * 1024)

    def test_a_size_that_is_not_known_stops(self):
        for text in ("Detected flash size: Unknown\n", "Detected flash size: 512KB\n", "Features: Embedded Flash 4MB (XMC)\n"):
            with self.assertRaises(flash.Stop):
                flash.flash_size_of(text)

    def test_mac(self):
        self.assertEqual(flash.mac_of(self.REAL), "000000000000")
        self.assertEqual(flash.mac_of("MAC:                12:34:56:78:9A:BC\nBASE MAC:           aa:aa:aa:aa:aa:aa\n"), "123456789abc")

    def test_a_mac_that_is_not_one_is_unknown(self):
        # it becomes part of the backup's file name
        for text in ("nothing here\n", "MAC: ../../somewhere\n", "MAC: 12:34:56\n", "MAC: 12:34:56:78:9a:bc:de:f0\n", "MAC: zz:zz:zz:zz:zz:zz\n"):
            self.assertEqual(flash.mac_of(text), "unknown", text)

    def test_chip(self):
        self.assertTrue(flash.chip_of(self.REAL).startswith("ESP32-C3"))
        self.assertEqual(flash.chip_of("Serial port /dev/esp32-c3:\nChip type:          ESP32-S3 (QFN56)\n"), "ESP32-S3 (QFN56)")
        self.assertEqual(flash.chip_of("Serial port /dev/esp32-c3:\n"), "")

    def test_the_port_is_found_when_exactly_one_espressif_port_is_there(self):
        def ports(*found):
            listing = types.ModuleType("serial.tools.list_ports")
            listing.comports = lambda: [types.SimpleNamespace(device=device, vid=vid) for device, vid in found]
            tools = types.ModuleType("serial.tools")
            tools.list_ports = listing
            serial = types.ModuleType("serial")
            serial.tools = tools
            return mock.patch.dict(sys.modules, {"serial": serial, "serial.tools": tools, "serial.tools.list_ports": listing})

        with ports(("COM3", 0x1A86), ("COM6", 0x303A)):
            self.assertEqual(flash.find_port(None), "COM6")
        with ports(("COM3", 0x1A86)):
            with self.assertRaisesRegex(flash.Stop, "no Espressif USB port"):
                flash.find_port(None)
        with ports(("COM6", 0x303A), ("COM7", 0x303A)):
            with self.assertRaisesRegex(flash.Stop, "more than one Espressif USB port: COM6, COM7"):
                flash.find_port(None)

    def test_only_a_plain_port_name_is_passed_on(self):
        for good in ("COM6", "COM12", "/dev/ttyACM0", "/dev/cu.usbmodem1101", "socket://127.0.0.1:5555"):
            self.assertEqual(flash.find_port(good), good)
        for bad in ("-p", "--erase-all", "@arguments.txt", "COM6 --erase-all", "COM6;x", ""):
            if bad:
                with self.assertRaisesRegex(flash.Stop, "not a port name"):
                    flash.find_port(bad)


class KnownImages(unittest.TestCase):
    def test_the_list_is_well_formed(self):
        listed = json.loads((ROOT / "tools" / "known_images.json").read_text(encoding="utf-8"))["images"]
        self.assertTrue(listed)
        for item in listed:
            self.assertRegex(item["sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue(item["what"].strip())
        self.assertEqual(len({item["sha256"] for item in listed}), len(listed))

    def test_an_image_is_named_by_its_checksum(self):
        raw = make_image()
        image = flash.image_of(raw, "x.bin")
        with tempfile.TemporaryDirectory() as tmp:
            listing = Path(tmp) / "known_images.json"
            listing.write_text(json.dumps({"images": [{"sha256": "0" * 64, "what": "another"}, {"sha256": image.sha256, "what": "the one"}]}), encoding="utf-8")
            with mock.patch.object(flash, "KNOWN_IMAGES", listing):
                self.assertEqual(flash.known(image), "the one")
                self.assertIsNone(flash.known(flash.image_of(make_image(version="9.9.9"), "y.bin")))

    def test_a_list_that_is_not_one_is_said_plainly(self):
        image = flash.image_of(make_image(), "x.bin")
        with tempfile.TemporaryDirectory() as tmp:
            listing = Path(tmp) / "known_images.json"
            for text in ("not json", "[]", "{}", '{"images": [{"sha": "x"}]}', '{"images": 5}', '{"images": ["x"]}'):
                listing.write_text(text, encoding="utf-8")
                with mock.patch.object(flash, "KNOWN_IMAGES", listing):
                    with self.assertRaisesRegex(flash.Stop, "not the list of images"):
                        flash.known(image)


class WholeCommand(unittest.TestCase):
    """tools/flash.py as a program, against a card that is a file."""

    FLASHING = ["flash-id", "read-flash", "read-flash", "verify-flash", "flash-id", "read-flash", "write-flash"]

    @classmethod
    def setUpClass(cls):
        # these tests run tools/flash.py for real: make sure the esptool it will start is the stand-in, not the one
        # that talks to hardware (a package beside it on the path would win over a folder without __init__.py)
        probe = subprocess.run([sys.executable, "-c", "import esptool; print(getattr(esptool, 'IS_STAND_IN', False))"],
                               capture_output=True, text=True, env=dict(os.environ, PYTHONPATH=str(HERE / "fake_esptool")))
        if probe.stdout.strip() != "True":
            raise RuntimeError("the stand-in esptool is not the one `python -m esptool` starts here: the tool was not run")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name).resolve()
        self.card = self.dir / "card-flash.bin"
        self.log = self.dir / "esptool-calls.jsonl"
        self.backups = self.dir / "backups"
        # a card as shipped: its table, a factory application, and everything else filled with bytes that are not ff,
        # so a write that went anywhere else would show
        filler = hashlib.sha256(b"filler").digest() * (FLASH_SIZE // 32)
        self.before = bytearray(filler)
        self.before[TABLE_AT] = fixture_table()
        self.stock = make_image(project="trae_card", version="1.2.2", payload=b"stock" * 9000)
        self.before[APP_OFFSET:APP_OFFSET + len(self.stock)] = self.stock
        self.card.write_bytes(self.before)
        self.image = self.dir / "clawhunt-agent-card-app.bin"
        self.image.write_bytes(make_image(payload=b"card" * 30000))

    def run_tool(self, *args, answer="", env=None, cwd=None):
        # the home folder is the test's own: a backup that goes to the default place must not land in a real one
        environment = dict(os.environ, PYTHONPATH=str(HERE / "fake_esptool"), FAKE_CARD_FLASH=str(self.card), FAKE_CARD_LOG=str(self.log),
                           HOME=str(self.dir), USERPROFILE=str(self.dir), **(env or {}))
        done = subprocess.run([sys.executable, str(ROOT / "tools" / "flash.py"), *args], input=answer, capture_output=True, text=True,
                              env=environment, cwd=cwd)
        return done.returncode, done.stdout + done.stderr

    def flash(self, *more, **kwargs):
        return self.run_tool("flash", str(self.image), "--port", PORT, "--backup-dir", str(self.backups), *more, **kwargs)

    def calls(self):
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text(encoding="utf-8").splitlines()]

    def commands(self):
        return [call["command"] for call in self.calls()]

    def writes(self):
        return [call for call in self.calls() if call["command"] == "write-flash"]

    def backup_files(self):
        return sorted(p.name for p in self.backups.iterdir()) if self.backups.exists() else []

    def assert_untouched(self):
        self.assertEqual(self.writes(), [])
        self.assertEqual(self.card.read_bytes(), bytes(self.before))

    def assert_only_the_application_changed(self, written: bytes):
        after = self.card.read_bytes()
        end = APP_OFFSET + len(written)
        erased_to = (end + 0xFFF) // 0x1000 * 0x1000     # a chip erases the whole 4 KB sectors that are written to
        self.assertEqual(len(after), FLASH_SIZE)
        self.assertEqual(after[:APP_OFFSET], bytes(self.before[:APP_OFFSET]), "bootloader, table, nvs or phy_init changed")
        self.assertEqual(after[APP_OFFSET:end], written)
        self.assertEqual(after[end:erased_to], b"\xff" * (erased_to - end), "the rest of the image's last sector is not erased flash")
        self.assertLessEqual(erased_to, APP_OFFSET + APP_SIZE, "the write reached past the application partition")
        self.assertEqual(after[erased_to:], bytes(self.before[erased_to:]), "something past the image's last sector changed")

    # ---- looking --------------------------------------------------------------------------------------------
    def test_check_reads_and_writes_nothing(self):
        code, out = self.run_tool("check", "--port", PORT)
        self.assertEqual(code, 0, out)
        self.assertIn("the firmware can be written to it", out)
        self.assertIn("project `trae_card`, version `1.2.2`", out)
        self.assertIn("MAC 123456789abc, flash 8 MB", out)
        self.assertEqual(self.commands(), ["flash-id", "read-flash", "read-flash", "read-mac"])
        self.assertEqual(self.calls()[-1]["after"], "hard-reset", "the card is not left in its loader")
        self.assertEqual(self.card.read_bytes(), bytes(self.before))

    def test_check_says_why_a_card_cannot_take_the_firmware(self):
        self.before[TABLE_AT] = make_table([e for e in factory_entries() if e[0] != "cardid"])
        self.card.write_bytes(self.before)
        code, out = self.run_tool("check", "--port", PORT)
        self.assertEqual(code, 1, "a script must be able to tell")
        self.assertIn("cannot take the firmware", out)
        self.assertIn("cardid", out)
        self.assertEqual(self.calls()[-1]["command"], "read-mac")
        self.assertEqual(self.card.read_bytes(), bytes(self.before))

    def test_check_on_a_card_without_an_application(self):
        self.before[APP_OFFSET:APP_OFFSET + 0x1000] = b"\xff" * 0x1000
        self.card.write_bytes(self.before)
        code, out = self.run_tool("check", "--port", PORT)
        self.assertEqual(code, 0, out)
        self.assertIn("on the card now: no application image", out)

    def test_a_chip_that_answers_and_then_fails_is_restarted(self):
        # esptool has put it into its loader by then: it must not be left there
        code, out = self.run_tool("check", "--port", PORT, env={"FAKE_CARD_FAIL_AFTER_CONNECT": "1"})
        self.assertEqual(code, 1, out)
        self.assertEqual(self.commands(), ["flash-id", "read-mac"])
        self.assertEqual(self.calls()[-1]["after"], "hard-reset")

    def test_plan_touches_no_card(self):
        code, out = self.run_tool("plan", str(self.image))
        self.assertEqual(code, 0, out)
        self.assertIn("nothing else is written", out)
        self.assertEqual(self.calls(), [])
        code, out = self.run_tool("plan", str(self.image), "--expect-version", "0.10.0")
        self.assertEqual(code, 0, out)
        code, out = self.run_tool("plan", str(self.image), "--expect-version", "9.9.9")
        self.assertEqual(code, 1, out)
        self.assertIn("not `9.9.9`", out)

    # ---- flashing -------------------------------------------------------------------------------------------
    def test_flash_backs_up_then_writes_only_the_application(self):
        code, out = self.flash("--yes")
        self.assertEqual(code, 0, out)
        calls = self.calls()
        self.assertEqual(self.commands(), self.FLASHING)
        self.assertEqual(calls[2]["args"][:2], ["0", hex(FLASH_SIZE)], "the backup is not the whole flash")
        self.assertEqual(calls[3]["args"][-2], "0", "the chip did not confirm the backup from the start of the flash")
        self.assertEqual(calls[3]["args"][-1], calls[2]["args"][2], "the chip confirmed another file than the backup")
        self.assertEqual(calls[6]["args"][0], "0x10000")
        self.assertEqual(len(calls[6]["args"]), 2)
        self.assertEqual(calls[6]["after"], "hard-reset")
        self.assertTrue(all(c["chip"] == "esp32c3" for c in calls))
        kept = list(self.backups.glob("card-123456789abc-*.bin"))
        self.assertEqual(len(kept), 1)
        self.assertEqual(self.backup_files(), [kept[0].name], "something besides the backup was left in its folder")
        self.assertEqual(kept[0].read_bytes(), bytes(self.before), "the backup is not the card as it was")
        self.assertIn("it holds the application that was on the card: project `trae_card`", out)
        self.assert_only_the_application_changed(self.image.read_bytes())

    def test_esptool_is_started_in_a_folder_of_the_tools_own(self):
        # python puts the folder it is started in on its module path: an esptool.py lying beside the user must not run
        stray = self.dir / "esptool.py"
        stray.write_text("raise SystemExit('the stray esptool.py ran')\n", encoding="utf-8")
        code, out = self.flash("--yes", cwd=str(self.dir))
        self.assertEqual(code, 0, out)
        folders = {c["cwd"] for c in self.calls()}
        self.assertEqual(len(folders), 1)
        folder = Path(folders.pop())
        self.assertNotEqual(folder.resolve(), self.dir)
        self.assertFalse(folder.exists(), "the tool's folder was left behind")

    def test_flash_asks_and_no_writes_nothing(self):
        for answer in ("no\n", "\n", "y\n", ""):
            self.log.unlink(missing_ok=True)
            code, out = self.flash(answer=answer)
            self.assertEqual(code, 1, out)
            self.assertIn("you did not say yes", out)
            self.assertNotIn("Traceback", out)
            self.assertEqual(self.calls()[-1]["command"], "read-mac", "the card is not restarted")
            self.assertEqual(self.calls()[-1]["after"], "hard-reset")
            self.assert_untouched()

    def test_flash_writes_after_yes_is_typed(self):
        code, out = self.flash(answer="yes\n")
        self.assertEqual(code, 0, out)
        self.assert_only_the_application_changed(self.image.read_bytes())

    def test_the_bytes_written_are_the_bytes_that_were_checked(self):
        # the image file is rewritten while the tool is at work (a build in another window): what was checked is written
        checked = self.image.read_bytes()
        code, out = self.flash("--yes", env={"FAKE_CARD_CHANGE_FILE": str(self.image)})
        self.assertEqual(code, 0, out)
        self.assertNotEqual(self.image.read_bytes(), checked, "the test did not change the file")
        self.assert_only_the_application_changed(checked)

    def test_what_follows_the_application_in_the_file_is_not_written(self):
        # only the application was checked: the rest of the file is nobody's business on the card
        checked = self.image.read_bytes()
        self.image.write_bytes(checked + b"JUNK" * 5000)
        code, out = self.flash("--yes")
        self.assertEqual(code, 0, out)
        self.assertIn("`flash` does not write them", out)
        self.assert_only_the_application_changed(checked)

    def test_any_image_lets_another_project_through(self):
        other = make_image(project="blink", payload=b"blink" * 3000)
        self.image.write_bytes(other)
        code, out = self.flash("--yes", "--any-image")
        self.assertEqual(code, 0, out)
        self.assert_only_the_application_changed(other)

    def test_a_backup_is_never_written_over(self):
        # the name a backup gets says the second it was made: take the names of the next half minute
        self.backups.mkdir()
        now = time.time()
        taken = [self.backups / f"card-123456789abc-{time.strftime('%Y%m%d-%H%M%S', time.localtime(now + s))}.bin" for s in range(-1, 30)]
        for name in taken:
            name.write_bytes(b"an earlier backup")
        code, out = self.flash("--yes")
        self.assertEqual(code, 0, out)
        self.assertEqual({name.read_bytes() for name in taken}, {b"an earlier backup"}, "an earlier backup was written over")
        new = [p for p in self.backups.iterdir() if p not in taken]
        self.assertEqual(len(new), 1, "the backup of this run is not beside the earlier ones under a name of its own")
        self.assertRegex(new[0].name, r"^card-123456789abc-\d{8}-\d{6}-2\.bin$")
        self.assertEqual(new[0].read_bytes(), bytes(self.before))
        # and such a backup is one `restore` takes
        self.log.unlink()
        code, out = self.run_tool("restore", str(new[0]), "--port", PORT, "--yes")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.card.read_bytes(), bytes(self.before))

    @unittest.skipUnless(os.name == "posix", "file modes are a POSIX matter")
    def test_a_backup_is_for_its_owner_only(self):
        code, out = self.flash("--yes")
        self.assertEqual(code, 0, out)
        kept = next(self.backups.glob("card-*.bin"))
        self.assertEqual(stat.S_IMODE(kept.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.backups.stat().st_mode), 0o700)

    def test_no_backup_skips_the_read_and_says_so(self):
        code, out = self.flash("--yes", "--no-backup")
        self.assertEqual(code, 0, out)
        self.assertIn("no backup taken", out)
        self.assertEqual(self.commands(), ["flash-id", "read-flash", "flash-id", "read-flash", "write-flash"])
        self.assertFalse(self.backups.exists())

    def test_the_backup_goes_to_the_home_folder_when_no_folder_is_named(self):
        code, out = self.run_tool("flash", str(self.image), "--port", PORT, "--yes")
        self.assertEqual(code, 0, out)
        kept = list((self.dir / "clawhunt-card-backups").glob("card-123456789abc-*.bin"))
        self.assertEqual(len(kept), 1)
        self.assertEqual(kept[0].read_bytes(), bytes(self.before))

    def test_no_stub_goes_to_every_esptool_command(self):
        code, out = self.flash("--yes", "--no-stub")
        self.assertEqual(code, 0, out)
        self.assertEqual([c["no_stub"] for c in self.calls()], [True] * len(self.FLASHING))
        self.assert_only_the_application_changed(self.image.read_bytes())
        self.log.unlink()
        code, out = self.run_tool("check", "--port", PORT)
        self.assertEqual(code, 0, out)
        self.assertEqual({c["no_stub"] for c in self.calls()}, {False})

    def test_names_with_spaces_and_an_at_sign_reach_esptool_as_whole_paths(self):
        # the real esptool reads an argument that starts with @ as a file of more arguments
        folder = self.dir / "my images"
        folder.mkdir()
        image = folder / "@card app.bin"
        image.write_bytes(self.image.read_bytes())
        code, out = self.run_tool("flash", "@card app.bin", "--port", PORT, "--yes", "--backup-dir", "@my backups", cwd=str(folder))
        self.assertEqual(code, 0, out)
        self.assertEqual(len(list((folder / "@my backups").glob("card-*.bin"))), 1)
        self.assert_only_the_application_changed(image.read_bytes())

    # ---- not flashing ---------------------------------------------------------------------------------------
    def test_a_card_without_cardid_is_not_touched(self):
        self.before[TABLE_AT] = make_table([e for e in factory_entries() if e[0] != "cardid"])
        self.card.write_bytes(self.before)
        code, out = self.flash("--yes")
        self.assertEqual(code, 1, out)
        self.assertIn("cardid", out)
        self.assertFalse(self.backups.exists(), "a backup was read from a card that is not an AI Passport")
        self.assert_untouched()

    def test_a_card_whose_application_partition_reaches_over_cardid_is_not_touched(self):
        entries = [(label, kind, subtype, offset, 0x400000 if label == "factory" else size) for label, kind, subtype, offset, size in factory_entries()]
        self.before[TABLE_AT] = make_table(entries)
        self.card.write_bytes(self.before)
        big = self.dir / "big.bin"
        big.write_bytes(make_image(payload=b"\x55" * 0x380000))
        for image in (self.image, big):
            code, out = self.run_tool("flash", str(image), "--port", PORT, "--yes", "--backup-dir", str(self.backups))
            self.assertEqual(code, 1, out)
            self.assertIn("not the AI Passport's factory layout", out)
            self.assert_untouched()

    def test_an_image_too_large_for_the_partition_is_not_written(self):
        big = self.dir / "big.bin"
        big.write_bytes(make_image(payload=b"\x55" * APP_SIZE))
        code, out = self.run_tool("flash", str(big), "--port", PORT, "--yes", "--backup-dir", str(self.backups))
        self.assertEqual(code, 1, out)
        self.assertIn("larger than the application partition", out)
        self.assert_untouched()

    def test_an_image_that_is_not_right_stops_before_the_card_is_touched(self):
        good = self.image.read_bytes()
        broken = bytearray(good)
        broken[5000] ^= 0x04
        cases = {
            "damaged": (bytes(broken), "damaged"),
            "another project": (make_image(project="blink"), "not this card's firmware"),
            "no hash of its own": (make_image(hashed=False) + b"\xff" * 64, "does not carry its own SHA-256"),
            "another chip": (make_image(chip_id=9), "not for the ESP32-C3"),
            "cut": (good[:len(good) // 2], "cut short"),
            "a whole flash, not an application": (bytes(self.before), "not an ESP application image"),
        }
        for what, (data, says) in cases.items():
            self.image.write_bytes(data)
            code, out = self.flash("--yes")
            self.assertEqual(code, 1, what)
            self.assertIn(says, out, what)
            self.assertEqual(self.calls(), [], what)

    def test_another_chip_on_the_port_is_not_written(self):
        # esptool itself refuses another chip than the one it was told to expect
        code, out = self.flash("--yes", env={"FAKE_CARD_CHIP": "ESP32-S3"})
        self.assertEqual(code, 1, out)
        self.assert_untouched()

    def test_the_tool_reads_the_chip_type_for_itself(self):
        # and were esptool ever to go on with another chip, the tool does not
        code, out = self.flash("--yes", env={"FAKE_CARD_CHIP_TYPE": "ESP32-S3 (QFN56) (revision v0.2)"})
        self.assertEqual(code, 1, out)
        self.assertIn("not an ESP32-C3", out)
        self.assertEqual(self.commands(), ["flash-id", "read-mac"])
        self.assert_untouched()

    def test_a_port_where_nothing_answers_is_asked_once(self):
        # a card that never answered was not stopped in what it was doing: the tool does not go on to "restart" it
        for command in (("check", "--port", PORT), ("flash", str(self.image), "--port", PORT, "--yes", "--backup-dir", str(self.backups))):
            self.log.unlink(missing_ok=True)
            code, out = self.run_tool(*command, env={"FAKE_CARD_NO_ANSWER": "1"})
            self.assertEqual(code, 1, out)
            self.assertIn("STOPPED: esptool failed", out)
            self.assertNotIn("could not restart", out)
            self.assertEqual(self.commands(), ["flash-id"])
            self.assert_untouched()

    def test_a_flash_of_another_size_is_not_written(self):
        for text, says in (("Unknown", "not a size this tool knows"), ("4MB", "an AI Passport has 8 MB"), ("16MB", "an AI Passport has 8 MB")):
            self.log.unlink(missing_ok=True)
            code, out = self.flash("--yes", env={"FAKE_CARD_SIZE_TEXT": text})
            self.assertEqual(code, 1, out)
            self.assertIn(says, out)
            self.assertFalse(self.backups.exists())
            self.assert_untouched()

    def test_a_card_that_cannot_be_told_from_another_is_not_written(self):
        code, out = self.flash("--yes", env={"FAKE_CARD_MAC_TEXT": "../../somewhere"})
        self.assertEqual(code, 1, out)
        self.assertIn("did not say the card's MAC", out)
        self.assertFalse(self.backups.exists())
        self.assert_untouched()

    def test_a_backup_that_is_not_the_flash_stops_the_write_and_is_not_kept(self):
        for fault in ("FAKE_CARD_BAD_READ", "FAKE_CARD_SHORT_READ"):
            self.log.unlink(missing_ok=True)
            code, out = self.flash("--yes", env={fault: "1"})
            self.assertEqual(code, 1, out)
            self.assertEqual(self.backup_files(), [], f"{fault}: a file that is not the flash was left where backups are kept")
            self.assertEqual(self.calls()[-1]["command"], "read-mac", "the card is not restarted")
            self.assert_untouched()

    def test_another_card_on_the_port_after_the_question_is_not_written(self):
        # flash-id, the table, the whole flash, the chip's confirmation: then somebody swaps the card
        code, out = self.flash("--yes", env={"FAKE_CARD_SWAP_AFTER": "4"})
        self.assertEqual(code, 1, out)
        self.assertIn("not the card that was checked", out)
        self.assertEqual(len(self.backup_files()), 1)
        self.assert_untouched()

    def test_a_write_that_stops_half_way_says_what_to_do(self):
        code, out = self.flash("--yes", env={"FAKE_CARD_FAIL_WRITE": "1"})
        self.assertEqual(code, 1, out)
        self.assertIn("THE WRITE DID NOT FINISH", out)
        self.assertIn("flash.py restore", out)
        self.assertEqual(self.calls()[-1]["command"], "read-mac", "the card is not restarted")
        after = self.card.read_bytes()
        self.assertNotEqual(after, bytes(self.before), "the stand-in wrote nothing")
        self.assertEqual(after[:APP_OFFSET], bytes(self.before[:APP_OFFSET]))
        self.assertEqual(after[APP_OFFSET + APP_SIZE:], bytes(self.before[APP_OFFSET + APP_SIZE:]))
        # and the backup taken before it brings the card back
        kept = next(self.backups.glob("card-*.bin"))
        code, out = self.run_tool("restore", str(kept), "--port", PORT, "--yes")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.card.read_bytes(), bytes(self.before))

    def test_a_port_name_that_esptool_would_read_as_something_else_is_refused(self):
        for port in ("@arguments.txt", "COM6 --erase-all"):
            code, out = self.run_tool("flash", str(self.image), f"--port={port}", "--yes", "--backup-dir", str(self.backups))
            self.assertEqual(code, 1, out)
            self.assertIn("not a port name", out)
            self.assertEqual(self.calls(), [])

    def test_a_missing_file_is_said_plainly(self):
        code, out = self.run_tool("flash", str(self.dir / "nothing.bin"), "--port", PORT, "--yes")
        self.assertEqual(code, 1, out)
        self.assertIn("no such file", out)
        self.assertNotIn("Traceback", out)
        self.assertEqual(self.calls(), [])

    # ---- restoring ------------------------------------------------------------------------------------------
    def flashed_and_its_backup(self) -> Path:
        code, out = self.flash("--yes")
        self.assertEqual(code, 0, out)
        self.assertNotEqual(self.card.read_bytes(), bytes(self.before))
        self.log.unlink()
        return next(self.backups.glob("card-*.bin"))

    def test_restore_puts_the_card_back_byte_for_byte(self):
        kept = self.flashed_and_its_backup()
        code, out = self.run_tool("restore", str(kept), "--port", PORT, "--yes")
        self.assertEqual(code, 0, out)
        self.assertIn("project `trae_card`, version `1.2.2`", out)
        self.assertEqual(self.commands(), ["flash-id", "read-flash", "flash-id", "read-flash", "write-flash"])
        write = self.writes()[0]
        self.assertEqual((write["args"][0], len(write["args"]), write["after"]), ("0x10000", 2, "hard-reset"))
        self.assertEqual(self.card.read_bytes(), bytes(self.before), "the card is not as it was before the flash")

    def test_restore_asks(self):
        kept = self.flashed_and_its_backup()
        flashed = self.card.read_bytes()
        code, out = self.run_tool("restore", str(kept), "--port", PORT, answer="no\n")
        self.assertEqual(code, 1, out)
        self.assertEqual(self.writes(), [])
        self.assertEqual(self.card.read_bytes(), flashed)

    def test_restore_says_when_the_backup_is_named_for_another_card(self):
        kept = self.flashed_and_its_backup()
        other = kept.with_name("card-aabbccddeeff-20260101-000000.bin")
        kept.rename(other)
        code, out = self.run_tool("restore", str(other), "--port", PORT, "--yes")
        self.assertEqual(code, 0, out)
        self.assertIn("by its name this backup is of the card aabbccddeeff", out)
        self.assertEqual(self.card.read_bytes(), bytes(self.before))

    def test_restore_refuses_a_file_that_is_not_a_backup_of_an_ai_passport(self):
        other = bytearray(self.before)
        other[TABLE_AT] = make_table([(l, k, s, o, 0x400000 if l == "factory" else z) for l, k, s, o, z in factory_entries()])
        no_app = bytearray(self.before)
        no_app[APP_OFFSET:APP_OFFSET + 64] = b"\xff" * 64
        cases = {
            "another layout": (bytes(other), "not a backup of an AI Passport"),
            "half a flash": (bytes(self.before[:FLASH_SIZE // 2]), "a whole-flash backup of an AI Passport is"),
            "an application image": (self.image.read_bytes(), "a whole-flash backup of an AI Passport is"),
            "no application in it": (bytes(no_app), "not an ESP application image"),
        }
        for what, (data, says) in cases.items():
            source = self.dir / "candidate.bin"
            source.write_bytes(data)
            code, out = self.run_tool("restore", str(source), "--port", PORT, "--yes")
            self.assertEqual(code, 1, what)
            self.assertIn(says, out, what)
            self.assertEqual(self.calls(), [], what)

    def test_restore_refuses_a_card_with_another_layout(self):
        backup = self.dir / "card-123456789abc-20260101-000000.bin"
        backup.write_bytes(self.before)
        self.before[TABLE_AT] = make_table(factory_entries() + [("extra", 1, 0x82, 0x4FA000, 0x1000)])
        self.card.write_bytes(self.before)
        code, out = self.run_tool("restore", str(backup), "--port", PORT, "--yes")
        self.assertEqual(code, 1, out)
        self.assertIn("not restoring", out)
        self.assert_untouched()

    def test_restore_does_not_write_to_another_card_than_the_one_it_looked_at(self):
        backup = self.dir / "card-123456789abc-20260101-000000.bin"
        backup.write_bytes(self.before)
        code, out = self.run_tool("restore", str(backup), "--port", PORT, "--yes", env={"FAKE_CARD_SWAP_AFTER": "2"})
        self.assertEqual(code, 1, out)
        self.assertIn("not the card that was checked", out)
        self.assert_untouched()


@unittest.skipUnless(os.environ.get("CARD_REAL_FILES"), "CARD_REAL_FILES is not set: the real backups stay on the machine they were taken on")
class RealFiles(unittest.TestCase):
    def setUp(self):
        self.folder = Path(os.environ["CARD_REAL_FILES"])

    def test_a_real_cards_table_is_the_factory_layout(self):
        whole = [d for d in sorted(self.folder.glob("stock-*.bin")) if d.stat().st_size == FLASH_SIZE]
        self.assertTrue(whole, "no whole-flash backup in CARD_REAL_FILES")
        for dump in whole:
            raw = dump.read_bytes()
            self.assertEqual(raw[TABLE_AT], fixture_table(), dump.name)
            parts = flash.parse_partition_table(raw[TABLE_AT])
            self.assertEqual(tuple(parts), flash.FACTORY_LAYOUT)
            self.assertEqual(flash.layout_problems(parts, 2_472_464), [])
            image = flash.image_of(raw[APP_OFFSET:APP_OFFSET + APP_SIZE], dump.name)
            self.assertEqual(image.chip_id, 5)

    def test_real_images_are_whole(self):
        images = sorted(self.folder.glob("app-*.bin"))
        self.assertTrue(images, "no application images in CARD_REAL_FILES")
        for path in images:
            raw = path.read_bytes()
            image = flash.read_image(path)
            self.assertEqual(image.project, "FoloToy-AI-Passport", path.name)
            # some of these are the image as built, some are the whole partition read back from the card; in the
            # second kind the end of an earlier, longer application can follow the image (only the written
            # sectors are erased), so what follows is not looked at
            self.assertEqual(image.file_size, len(raw), path.name)
            self.assertEqual(image.sha256, hashlib.sha256(raw[:image.size]).hexdigest(), path.name)


if __name__ == "__main__":
    unittest.main()
