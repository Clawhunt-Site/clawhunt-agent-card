"""Render the card's chimes to WAV files so they can be heard on a computer.

    python tools/sfx_preview.py <out-dir>

Same synthesis as main/sfx.c (keep the two tables in step): a sine with two quiet
overtones, 4 ms of attack and an exponential decay. A computer speaker is kinder
than the card's small one, so treat this as a guide to the melody, not the tone.
"""
import math
import pathlib
import struct
import sys
import wave

RATE = 16000
PEAK = 9000
ATTACK = 64
CHIMES = {   # name: [(hz, ms, decay_ms, level), ...]
    "blip": [(1319, 70, 28, 110)],
    "ring": [(784, 170, 90, 220), (1047, 170, 90, 220), (1319, 460, 170, 255)],
    "coin": [(988, 95, 60, 220), (1319, 560, 210, 255)],
    "level_up": [(523, 120, 80, 220), (659, 120, 80, 220), (784, 120, 80, 220), (1047, 620, 240, 255)],
    "lose": [(659, 190, 110, 190), (523, 460, 190, 190)],
    "paid_out": [(1319, 120, 70, 230), (1568, 600, 230, 255)],
}


def note(hz: int, ms: int, decay_ms: int, level: int) -> list:
    total = RATE * ms // 1000
    decay = math.exp(-1000.0 / (decay_ms * RATE))
    envelope = level / 255
    samples = []
    for i in range(total):
        angle = 2 * math.pi * hz * i / RATE
        wave_value = (math.sin(angle) * 180 + math.sin(angle * 2) * 56 + math.sin(angle * 3) * 20) / 256
        sample = wave_value * envelope * PEAK
        if i < ATTACK:
            sample = sample * i / ATTACK
        samples.append(int(sample))
        envelope *= decay
    return samples


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, notes in CHIMES.items():
        samples = [s for spec in notes for s in note(*spec)]
        with wave.open(str(out / f"{name}.wav"), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(RATE)
            wav.writeframes(struct.pack(f"<{len(samples)}h", *samples))
        print(f"{name}.wav  {len(samples) / RATE:.2f} s  peak {max(abs(s) for s in samples)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
