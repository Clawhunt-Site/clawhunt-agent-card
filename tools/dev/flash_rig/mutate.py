"""Do the tests of tools/flash.py notice when the tool is broken?

    python mutate.py <repo checkout>

One safety property at a time is taken out of a COPY of tools/flash.py (the checkout is never changed), the tests
are run on the copy, and a test must go red. A change the tests do not notice is a hole in the tests. A change
whose text is not found in flash.py any more (the tool was edited) is reported too: bring this list up to date.
About 40 seconds per change.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

CHANGES = [
    ("writes from 0x0",
     'esptool(port, "write-flash", hex(APP_OFFSET), str(staged), after="hard-reset")',
     'esptool(port, "write-flash", hex(0), str(staged), after="hard-reset")'),
    ("card not checked before flashing",
     '        if problems:\n            raise Stop("not flashing:',
     '        if False:\n            raise Stop("not flashing:'),
    ("no backup",
     '            kept = backup(port, found, Path(args.backup_dir))',
     '            kept = Path("none")'),
    ("does not ask",
     '    if answer.strip().lower() != "yes":',
     '    if False:'),
    ("any layout with an application at 0x10000 and a cardid is accepted",
     '    if not problems and tuple(parts) != FACTORY_LAYOUT:',
     '    if False:'),
    ("image hash not checked",
     '    if hashlib.sha256(raw[:end]).digest() != raw[end:end + 32]:',
     '    if False:'),
    ("image without its own hash accepted",
     '    if raw[23] != 1:',
     '    if False:'),
    ("restore writes the whole backup",
     '        write_application(port, found, piece, state, "run it again")',
     '        write_application(port, found, full, state, "run it again")'),
    ("card not restarted after a stop",
     '        if state["restart"] and _REACHED:',
     '        if False:'),
    ("a card that never answered is restarted anyway",
     '        if state["restart"] and _REACHED:',
     '        if state["restart"]:'),
    ("a list of known images that is not one is a traceback",
     '    except (ValueError, LookupError, TypeError) as bad:',
     '    except ZeroDivisionError as bad:'),
    ("project not checked",
     '    if image.project != EXPECTED_PROJECT and not any_image:',
     '    if False:'),
    ("partition table checksum not checked",
     '            if entry[2:16] != b"\\xff" * 14 or hashlib.md5(raw[:i], usedforsecurity=False).digest() != entry[16:32]:',
     '            if False:'),
    ("a table without a checksum entry accepted",
     '    if parts:\n        raise Stop("the partition table has no checksum entry',
     '    if False:\n        raise Stop("the partition table has no checksum entry'),
    ("the file is read again for the write",
     '        write_application(port, found, application, state, undo)',
     '        write_application(port, found, path.read_bytes()[:image.size], state, undo)'),
    ("what follows the application in the file is written too",
     '    application = raw[:image.size]      # what was checked, and nothing that follows it in the file',
     '    application = raw'),
    ("check does not say no to a card that cannot take the firmware",
     '        if problems:        # exit 1, so that a script can tell\n            raise Stop("this card cannot take the firmware as it is:',
     '        if False:\n            raise Stop("this card cannot take the firmware as it is:'),
    ("a chip that answered and then failed is left in its loader",
     '    if "Connected to" in out:',
     '    if code == 0:'),
    ("a backup is written over",
     '    while final.exists() or (folder / (final.name + ".partial")).exists():     # a backup is never written over',
     '    while False:'),
    ("the tool does not read the chip type for itself",
     '    if not chip_of(info).upper().startswith("ESP32-C3"):',
     '    if False:'),
    ("backup not confirmed by the chip",
     '        esptool(port, "verify-flash", "--flash-mode", "keep", "--flash-freq", "keep", "--flash-size", "keep", "0", str(partial))',
     '        pass'),
    ("backup size not checked",
     '        if len(raw) != FLASH_SIZE:\n            raise Stop(f"the backup is',
     '        if False:\n            raise Stop(f"the backup is'),
    ("card not looked at again before the write",
     '    if look(port) != found:',
     '    if False:'),
    ("flash size not required to be 8 MB",
     '    if found.flash_size != FLASH_SIZE:',
     '    if False:'),
    ("a card without a readable MAC accepted",
     '    if found.mac == "unknown":',
     '    if False:'),
    ("a failed backup left behind",
     '        try:\n            partial.unlink()\n        except OSError:\n            pass\n        raise',
     '        raise'),
    ("esptool started in the user's folder",
     'env=env, cwd=str(workdir()),',
     'env=env, cwd=None,'),
    ("restore does not check the backup's layout",
     '    if problems:\n        raise Stop(f"{source.name} is not a backup of an AI Passport',
     '    if False:\n        raise Stop(f"{source.name} is not a backup of an AI Passport'),
    ("restore does not check the card",
     '        if problems:\n            raise Stop("not restoring:',
     '        if False:\n            raise Stop("not restoring:'),
    ("port name not checked",
     '        if not re.fullmatch(r"[A-Za-z0-9/][A-Za-z0-9_./:-]*", asked):',
     '        if False:'),
    ("--no-stub not passed on",
     '    ESPTOOL_OPTIONS[:] = ["--no-stub"] if getattr(args, "no_stub", False) else []',
     '    ESPTOOL_OPTIONS[:] = []'),
    ("version not insisted on",
     '    if expect_version is not None and image.version != expect_version:',
     '    if False:'),
    ("an image larger than the partition accepted",
     '    if file_size > APP_SIZE:',
     '    if False:'),
    ("the hint after a failed write is not printed",
     '        print("THE WRITE DID NOT FINISH.',
     '        (lambda *a, **k: None)("THE WRITE DID NOT FINISH.'),
    ("no answer at the prompt is a traceback",
     '    except EOFError:\n        answer = ""',
     '    except ZeroDivisionError:\n        answer = ""'),
]


def copy_of(repo: pathlib.Path, work: pathlib.Path) -> None:
    """What the tests need, and nothing else of the checkout."""
    if work.exists():
        shutil.rmtree(work)
    (work / "tools").mkdir(parents=True)
    (work / "firmware").mkdir()
    shutil.copyfile(repo / "tools" / "flash.py", work / "tools" / "flash.py")
    shutil.copyfile(repo / "tools" / "known_images.json", work / "tools" / "known_images.json")
    shutil.copytree(repo / "tools" / "tests", work / "tools" / "tests", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copyfile(repo / "firmware" / "partitions.csv", work / "firmware" / "partitions.csv")


def tests(work: pathlib.Path):
    done = subprocess.run([sys.executable, "-X", "utf8", "-m", "unittest", "discover", "-s", "tools/tests"], cwd=work,
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    return done.returncode, (done.stderr.strip().splitlines() or ["(nothing printed)"])[-1]


def main() -> int:
    repo = pathlib.Path(sys.argv[1]).resolve()
    original = (repo / "tools" / "flash.py").read_text(encoding="utf-8")
    holes = 0
    with tempfile.TemporaryDirectory(prefix="flash-mutate-") as tmp:
        work = pathlib.Path(tmp) / "copy"
        copy_of(repo, work)
        code, last = tests(work)
        print(f"the tool as it is: {last}", flush=True)
        if code != 0:
            print("the tests do not pass on the unchanged tool: nothing to learn from breaking it")
            return 1
        for name, old, new in CHANGES:
            if original.count(old) != 1:
                print(f"NOT TRIED  {name}: its text is in flash.py {original.count(old)} times", flush=True)
                holes += 1
                continue
            copy_of(repo, work)
            (work / "tools" / "flash.py").write_bytes(original.replace(old, new).encode("utf-8"))
            code, last = tests(work)
            holes += 0 if code else 1
            print(f"{'caught' if code else 'NOT CAUGHT':10s} {name}: {last}", flush=True)
    print(f"{len(CHANGES)} changes, {holes} not caught or not tried")
    return 1 if holes else 0


if __name__ == "__main__":
    sys.exit(main())
