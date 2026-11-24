"""Feed the ClawHunt agent card over USB and look at its screen.

    uv run --with pyserial --with pillow tools/feed.py demo            # pretend marketplace
    uv run --with pyserial --with pillow tools/feed.py shot out.png    # screenshot
    uv run --with pyserial --with pillow tools/feed.py verify out_dir  # scripted walk-through

The numbers in `demo` and `verify` are made up for the demo; nothing here talks to
ClawHunt. The wire format is documented in main/host_link.h.
"""
import argparse
import itertools
import json
import pathlib
import sys
import time

import serial
import serial.tools.list_ports
from PIL import Image

ESPRESSIF_VID = 0x303A
SHOT_COMMAND = b"FAP_SCREENSHOT_V1"
FEE_BPS = 2500          # the platform keeps 25 %
XP_PER_DEAL = 35
JOBS = [("写个爬虫", 2500), ("修支付回调", 4000), ("做数据看板", 6000), ("翻译产品文档", 1800)]


def find_port() -> str:
    ports = [p.device for p in serial.tools.list_ports.comports() if p.vid == ESPRESSIF_VID]
    if not ports:
        sys.exit("no Espressif USB serial port found; is the card plugged in and switched on?")
    return ports[0]


class Card:
    """One open serial connection to the card."""

    def __init__(self, port: str):
        # Leave DTR/RTS low: on the ESP32-C3 USB bridge raising them resets the chip.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.05
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.pending = b""

    def send(self, **message) -> None:
        line = "@" + json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n"
        self.ser.write(line.encode("utf-8"))
        self.ser.flush()

    def poll(self, seconds: float) -> list:
        """Collect the card's messages for a while. Non-protocol lines are ignored."""
        deadline = time.monotonic() + seconds
        messages = []
        while time.monotonic() < deadline:
            self.pending += self.ser.read(256)
            while b"\n" in self.pending:
                raw, self.pending = self.pending.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip()
                if not text.startswith("@"):
                    continue
                try:
                    messages.append(json.loads(text[1:]))
                except json.JSONDecodeError:
                    pass
        return messages

    def wait_for(self, kinds, seconds: float):
        """First message whose type is in kinds (a name or a tuple of names), or None."""
        wanted = (kinds,) if isinstance(kinds, str) else kinds
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            for message in self.poll(0.1):
                if message.get("t") in wanted:
                    return message
        return None

    def screenshot(self) -> Image.Image:
        self.poll(0.1)
        self.pending = b""
        self.ser.reset_input_buffer()
        self.ser.write(SHOT_COMMAND + b"\n")
        self.ser.flush()
        data = b""
        deadline = time.monotonic() + 15
        header = None
        while time.monotonic() < deadline:
            data += self.ser.read(4096)
            if header is None:
                start = data.find(SHOT_COMMAND + b" ")
                end = data.find(b"\n", start) if start >= 0 else -1
                if end < 0:
                    continue
                header = data[start:end].decode("ascii").split()
                data = data[end + 1 :]
            width, height, size = int(header[1]), int(header[2]), int(header[4])
            if len(data) >= size:
                self.pending = data[size:]
                return rgb565_to_image(data[:size], width, height)
        raise TimeoutError("the card did not finish sending a screenshot")


def rgb565_to_image(raw: bytes, width: int, height: int) -> Image.Image:
    pixels = bytearray(width * height * 3)
    for i in range(width * height):
        value = raw[2 * i] | (raw[2 * i + 1] << 8)
        r, g, b = (value >> 11) & 0x1F, (value >> 5) & 0x3F, value & 0x1F
        pixels[3 * i : 3 * i + 3] = bytes(((r * 255) // 31, (g * 255) // 63, (b * 255) // 31))
    return Image.frombytes("RGB", (width, height), bytes(pixels))


class Market:
    """A pretend marketplace: holds the agent's numbers and plays out one job."""

    def __init__(self, card: Card):
        self.card = card
        self.stats = dict(name="polyglot-dev", lvl=7, xp=70, deals=39, rating=49, streak=6, today=4250)
        self.ids = itertools.count(1432)

    def push_stats(self) -> None:
        self.card.send(t="stats", **self.stats)

    def offer(self, title: str, amount: int, ttl: int = 6000, direct: bool = False) -> int:
        job_id = next(self.ids)
        self.card.send(t="offer", id=job_id, title=title, amt=amount, direct=int(direct), ttl=ttl)
        return job_id

    def run_job(self, job_id: int, title: str, amount: int, step: float = 0.11) -> None:
        time.sleep(0.9)
        self.card.send(t="job", s="won")
        time.sleep(0.7)
        for progress in range(4, 101, 4):
            self.card.send(t="job", s="work", p=progress)
            time.sleep(step)
        self.card.send(t="job", s="done")
        time.sleep(1.0)
        self.pay(job_id, title, amount)

    def pay(self, job_id: int, title: str, amount: int) -> None:
        net = amount * (10000 - FEE_BPS) // 10000
        self.stats["deals"] += 1
        self.stats["streak"] += 1
        self.stats["today"] += net
        self.stats["xp"] += XP_PER_DEAL
        new_level = 0
        if self.stats["xp"] >= 100:
            self.stats["xp"] -= 100
            self.stats["lvl"] += 1
            new_level = self.stats["lvl"]
        self.card.send(t="paid", id=job_id, title=title, net=net, lvl=new_level)
        self.push_stats()


def cmd_demo(card: Card, args) -> None:
    market = Market(card)
    market.push_stats()
    print("feeding the card; press OK on it within 6 s of each offer. Ctrl+C to stop.")
    for title, amount in itertools.cycle(JOBS):
        time.sleep(args.gap)
        job_id = market.offer(title, amount)
        print(f"offer #{job_id} {title} ${amount / 100:.2f}")
        if args.auto_accept:
            time.sleep(1.5)
            card.send(t="key", k="ok")
        answer = card.wait_for(("accept", "miss"), 8)
        if not answer or answer["t"] == "miss":
            market.stats["streak"] = 0
            market.push_stats()
            print("  missed")
            time.sleep(2)
            continue
        print("  accepted, working")
        market.run_job(job_id, title, amount)
        print(f"  paid, today ${market.stats['today'] / 100:.2f}, level {market.stats['lvl']}")
        time.sleep(5)


def cmd_shot(card: Card, args) -> None:
    card.screenshot().save(args.out)
    print(f"saved {args.out}")


def cmd_verify(card: Card, args) -> None:
    """Drive every screen once, pressing keys from here, and save a picture of each."""
    out = pathlib.Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    report = {}

    def snap(name: str) -> None:
        card.screenshot().save(out / f"{name}.png")

    card.send(t="ping")
    report["hello"] = card.wait_for("hello", 3)
    market = Market(card)
    market.push_stats()
    time.sleep(0.5)
    snap("1-idle")

    job_id = market.offer("写个爬虫", 2500)
    time.sleep(0.8)
    snap("2-offer")
    card.send(t="key", k="ok")
    report["accept"] = card.wait_for("accept", 3)
    card.send(t="job", s="won")
    card.send(t="job", s="work", p=60)
    time.sleep(0.5)
    snap("3-working")
    card.send(t="job", s="done")
    market.pay(job_id, "写个爬虫", 2500)
    time.sleep(0.9)
    snap("4-paid")
    time.sleep(2.2)
    snap("5-level-up")
    time.sleep(2.6)
    snap("6-idle-after")

    missed = market.offer("修支付回调", 4000, ttl=1500, direct=True)
    time.sleep(0.6)
    snap("7-direct-offer")
    report["miss"] = card.wait_for("miss", 4)
    report["miss_expected_id"] = missed
    time.sleep(0.4)
    snap("8-missed")
    time.sleep(2.0)
    snap("9-idle-final")
    print(json.dumps(report, ensure_ascii=False, indent=1))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--port", help="serial port; default: the first Espressif USB device")
    sub = parser.add_subparsers(dest="command", required=True)
    demo = sub.add_parser("demo")
    demo.add_argument("--gap", type=float, default=4.0, help="seconds between offers")
    demo.add_argument("--auto-accept", action="store_true", help="press OK from the computer")
    demo.set_defaults(run=cmd_demo)
    shot = sub.add_parser("shot")
    shot.add_argument("out")
    shot.set_defaults(run=cmd_shot)
    verify = sub.add_parser("verify")
    verify.add_argument("out_dir")
    verify.set_defaults(run=cmd_verify)
    args = parser.parse_args()

    card = Card(args.port or find_port())
    try:
        args.run(card, args)
    except KeyboardInterrupt:
        print("stopped")
    finally:
        card.ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
