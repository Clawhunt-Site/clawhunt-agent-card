"""A stand-in for esptool 5 in the tests of tools/flash.py: a "card" that is a file.

It takes the command lines flash.py gives esptool, does them to the file named by FAKE_CARD_FLASH, prints what
esptool 5.6 prints for them (the lines flash.py reads were copied from real runs), and appends each call to
FAKE_CARD_LOG as one JSON line.

It knows only what flash.py uses (flash-id, read-flash, verify-flash, write-flash, read-mac) and is strict about
it: an option or a command it does not know fails, loudly, and so does an argument that starts with @, which the
real esptool would take for a file of arguments.

Things a test can make go wrong, by environment variable:
  FAKE_CARD_NO_ANSWER=1            nothing on the port answers
  FAKE_CARD_FAIL_AFTER_CONNECT=1   the chip answers, then `flash-id` fails
  FAKE_CARD_CHIP=ESP32-S3          another chip is on the port, and esptool says so and fails
  FAKE_CARD_CHIP_TYPE=...          what esptool prints as the chip type, without failing
  FAKE_CARD_SIZE_TEXT=4MB          what `flash-id` says the flash size is
  FAKE_CARD_MAC_TEXT=...           what is printed as the MAC
  FAKE_CARD_SWAP_AFTER=4           from the fifth call on, the card is another one (another MAC)
  FAKE_CARD_BAD_READ=1             a whole-flash read comes back with one byte wrong
  FAKE_CARD_SHORT_READ=1           a whole-flash read comes back one byte short
  FAKE_CARD_FAIL_WRITE=1           a write stops half way and esptool fails
  FAKE_CARD_CHANGE_FILE=path       on the first call, that file is overwritten (somebody rebuilt the image meanwhile)
"""
import hashlib
import json
import os
import sys

MAC = "12:34:56:78:9a:bc"
OTHER_MAC = "aa:bb:cc:dd:ee:ff"
SECTOR = 0x1000
GLOBAL_VALUES = ("--chip", "--port", "--after")
GLOBAL_FLAGS = ("--no-stub",)
KEEP = ("--flash-mode", "--flash-freq", "--flash-size")


def fatal(message):
    print(f"\nA fatal error occurred: {message}")
    return 2


def main(argv):
    flash_path = os.environ["FAKE_CARD_FLASH"]
    log_path = os.environ["FAKE_CARD_LOG"]
    if any(arg.startswith("@") for arg in argv):
        return fatal("an argument starts with @: the real esptool reads it as a file of arguments")
    opts = {}
    while argv and argv[0].startswith("--"):
        if argv[0] in GLOBAL_FLAGS:
            opts[argv[0]] = True
            argv = argv[1:]
        elif argv[0] in GLOBAL_VALUES:
            opts[argv[0]] = argv[1]
            argv = argv[2:]
        else:
            return fatal(f"the stand-in does not know the option {argv[0]}")
    command, args = argv[0], argv[1:]
    try:
        with open(log_path, encoding="utf-8") as log:
            before = len(log.readlines())
    except FileNotFoundError:
        before = 0
    with open(log_path, "a", encoding="utf-8") as log:
        log.write(json.dumps({"command": command, "args": args, "after": opts.get("--after"), "chip": opts.get("--chip"),
                              "no_stub": opts.get("--no-stub", False), "cwd": os.getcwd()}) + "\n")
    if before == 0 and os.environ.get("FAKE_CARD_CHANGE_FILE"):
        with open(os.environ["FAKE_CARD_CHANGE_FILE"], "wb") as fh:
            fh.write(b"not the image that was checked" * 100)
    chip = os.environ.get("FAKE_CARD_CHIP", "ESP32-C3")
    mac = os.environ.get("FAKE_CARD_MAC_TEXT", MAC)
    swap_after = os.environ.get("FAKE_CARD_SWAP_AFTER")
    if swap_after is not None and before >= int(swap_after):
        mac = OTHER_MAC
    print("esptool v5.6.0")
    print(f"Serial port {opts.get('--port')}:")
    print("Connecting....")
    if os.environ.get("FAKE_CARD_NO_ANSWER"):
        return fatal("Failed to connect to ESP32-C3: No serial data received.")
    if chip != "ESP32-C3":
        return fatal(f"This chip is {chip}, not ESP32-C3. Wrong chip argument?")
    print(f"Connected to ESP32-C3 on {opts.get('--port')}:")
    print(f"{'Chip type:':<20}{os.environ.get('FAKE_CARD_CHIP_TYPE', 'ESP32-C3 (QFN32) (revision v0.4)')}")
    print(f"{'Features:':<20}Wi-Fi, BT 5 (LE), Single Core, 160MHz, Embedded Flash 4MB (XMC)")
    print(f"{'Crystal frequency:':<20}40MHz")
    print(f"{'USB mode:':<20}USB-Serial/JTAG")
    print(f"{'MAC:':<20}{mac}")
    print()
    with open(flash_path, "rb") as fh:
        flash = bytearray(fh.read())
    if command == "flash-id":
        if args:
            return fatal("flash-id takes no arguments")
        if os.environ.get("FAKE_CARD_FAIL_AFTER_CONNECT"):
            return fatal("Failed to read the flash id (the chip answered, then stopped answering)")
        print("Flash Memory Information:")
        print("=========================")
        print("Manufacturer: c8")
        print("Device: 4017")
        print(f"Detected flash size: {os.environ.get('FAKE_CARD_SIZE_TEXT') or str(len(flash) // (1024 * 1024)) + 'MB'}")
    elif command == "read-flash":
        if len(args) != 3:
            return fatal("read-flash takes an address, a size and a file")
        offset, size, out = int(args[0], 0), int(args[1], 0), args[2]
        data = bytearray(flash[offset:offset + size])
        if size == len(flash) and os.environ.get("FAKE_CARD_BAD_READ"):
            data[len(data) // 3] ^= 0x40
        if size == len(flash) and os.environ.get("FAKE_CARD_SHORT_READ"):
            data = data[:-1]
        with open(out, "wb") as fh:
            fh.write(data)
        print(f"Read {size} bytes from {offset:#010x} in 0.1 seconds (1.0 kbit/s) to '{out}'.")
    elif command == "verify-flash":
        while args and args[0] in KEEP:
            if args[1] != "keep":
                return fatal("the stand-in only knows `keep` for the flash options")
            args = args[2:]
        if len(args) != 2:
            return fatal("verify-flash takes an address and a file")
        offset, source = int(args[0], 0), args[1]
        with open(source, "rb") as fh:
            data = fh.read()
        print(f"Verifying {len(data):#x} ({len(data)}) bytes at {offset:#010x} in flash against '{source}'...")
        if hashlib.md5(bytes(flash[offset:offset + len(data)])).digest() != hashlib.md5(data).digest():
            print("Verification failed (digest mismatch).")
            return fatal("Verification failed.")
        print("Verification successful (digest matched).")
    elif command == "write-flash":
        if len(args) != 2:
            return fatal("write-flash takes an address and a file")
        offset, source = int(args[0], 0), args[1]
        with open(source, "rb") as fh:
            data = fh.read()
        if offset + len(data) > len(flash):
            return fatal("the file does not fit in the flash")
        # as on a chip: the 4 KB sectors the data touches are erased (all ff) before it is written
        erased_to = min(len(flash), (offset + len(data) + SECTOR - 1) // SECTOR * SECTOR)
        flash[offset:erased_to] = b"\xff" * (erased_to - offset)
        if os.environ.get("FAKE_CARD_FAIL_WRITE"):
            half = len(data) // 2
            flash[offset:offset + half] = data[:half]
            with open(flash_path, "wb") as fh:
                fh.write(flash)
            return fatal("No more data to read from the serial port.")
        flash[offset:offset + len(data)] = data
        with open(flash_path, "wb") as fh:
            fh.write(flash)
        print(f"Wrote {len(data)} bytes ({len(data)} compressed) at {offset:#010x} in 0.1 seconds (1.0 kbit/s).")
        print("Hash of data verified.")
    elif command == "read-mac":
        if args:
            return fatal("read-mac takes no arguments")
    else:
        return fatal(f"the stand-in does not know the command {command}")
    print()
    print("Hard resetting via RTS pin..." if opts.get("--after") == "hard-reset" else "Staying in bootloader.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
