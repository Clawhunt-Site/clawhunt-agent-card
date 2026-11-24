"""After tools/flash.py ran against the emulated card: is the backup the original flash, and what did the run change?

    compare.py <original flash> <flash after the run> <backup the tool kept> [<image that was written>]

With the image: the application partition must start with it. Without (a write that stopped half way): only where
the changes are is said.
"""
import sys

original, after, backup = (open(p, "rb").read() for p in sys.argv[1:4])
image = open(sys.argv[4], "rb").read() if len(sys.argv) > 4 else None
APP, APP_END = 0x10000, 0x310000

print("backup == original flash:", backup == original, f"({len(backup)} bytes)")
print("sizes equal:", len(original) == len(after))
changed = [i for i in range(min(len(original), len(after))) if original[i] != after[i]]
print("bytes of the flash that changed:", len(changed))
if changed:
    print(f"  first 0x{changed[0]:x}, last 0x{changed[-1]:x}")
    print("  all inside the application partition [0x10000, 0x310000):", APP <= changed[0] and changed[-1] < APP_END)
print("before the application partition unchanged (bootloader, table, nvs, phy_init):", after[:APP] == original[:APP])
print("after the application partition unchanged (imgstore, imgframe, cardid, imguser, audio, imgava, the rest):", after[APP_END:] == original[APP_END:])
if image is not None:
    print("the application partition starts with the image written:", after[APP:APP + len(image)] == image, f"({len(image)} bytes)")
    past = sum(1 for a, b in zip(after[APP + len(image):APP_END], original[APP + len(image):APP_END]) if a != b)
    print("the application partition past the image:", "as before" if past == 0 else f"changed in {past} bytes (sectors erased for the write)")
