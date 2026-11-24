"""USB gateway: puts a real ClawHunt agent on the card and lets a phone ask it to pay.

    uv run --with pyserial --with pillow tools/gateway.py            # this computer only
    uv run --with pyserial --with pillow tools/gateway.py --lan      # reachable from a phone

What is real and what is not:
  * The agent's name, level and delivery counts are read live from ClawHunt's
    public API (read-only, no login, no key).
  * The wallet is a SANDBOX kept in a local JSON file. It is seeded once with the
    agent's public lifetime earnings so the numbers look familiar, but paying
    from it moves no real money and never touches ClawHunt's ledger.

The page at /c/<agent-id> is what an NFC tap on the card should open. Anyone who
opens it can ask the card for money; nothing is paid until the holder presses OK
on the card, and the gateway enforces a per-payment and a daily limit.
"""
import argparse
import datetime
import io
import json
import pathlib
import queue
import random
import re
import sys
import threading
import time
import urllib.request
from decimal import Decimal, InvalidOperation
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import feed

ROOT = pathlib.Path(__file__).resolve().parent.parent
PAGE = pathlib.Path(__file__).with_name("gateway_page.html")
SETUP_PAGE = pathlib.Path(__file__).with_name("gateway_setup.html")
ASSETS = pathlib.Path(__file__).with_name("assets")
LOCAL_ADDRESSES = ("127.0.0.1", "::1")
USER_AGENT = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0 Safari/537.36 ClawHuntCardTools/0.10"
)
LADDER = [2, 5, 10, 20]        # accepted deliveries needed for levels 2..5, as the site counts them
PER_PAYMENT_LIMIT = 5000       # cents
DAILY_LIMIT = 20000            # cents
PAY_TTL_MS = 20000
PAYEE_MAX_BYTES = 23
MEMO_MAX_BYTES = 60


def fetch_json(url: str) -> dict:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.loads(response.read().decode("utf-8"))


def ladder_progress(tasks: int) -> int:
    """Percent of the way from the current level to the next one."""
    previous = 0
    for threshold in LADDER:
        if tasks < threshold:
            return (tasks - previous) * 100 // (threshold - previous)
        previous = threshold
    return 100


def clip_utf8(text: str, max_bytes: int) -> str:
    """Printable text only, cut on a character boundary to fit the card's fields."""
    text = "".join(ch for ch in text if ch.isprintable()).strip()
    return text.encode("utf-8")[:max_bytes].decode("utf-8", "ignore")


def parse_amount(raw) -> int:
    """Dollars as text ("5", "5.50") to cents. Raises ValueError on anything else."""
    if not isinstance(raw, str) or not re.fullmatch(r"\d{1,6}(\.\d{1,2})?", raw.strip()):
        raise ValueError("amount must look like 5 or 5.50")
    try:
        return int(Decimal(raw.strip()) * 100)
    except InvalidOperation as exc:
        raise ValueError("amount is not a number") from exc


class Ledger:
    """The sandbox wallet. One JSON file; every change is written straight away."""

    def __init__(self, path: pathlib.Path):
        self.path = path
        self.data = {"balance": None, "seeded_from": None, "spent": {}, "history": []}
        if path.exists():
            self.data.update(json.loads(path.read_bytes().decode("utf-8")))

    def seed(self, cents: int, source: str) -> None:
        if self.data["balance"] is None:
            self.data.update(balance=cents, seeded_from=source)
            self.save()

    def save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_bytes(json.dumps(self.data, ensure_ascii=False, indent=1).encode("utf-8"))

    @property
    def balance(self) -> int:
        return self.data["balance"] or 0

    def spent_today(self) -> int:
        return self.data["spent"].get(datetime.date.today().isoformat(), 0)

    def check(self, cents: int) -> str:
        """Why this payment may not even be asked for, or '' if it may."""
        if cents <= 0:
            return "金额要大于 0"
        if cents > PER_PAYMENT_LIMIT:
            return f"单笔最多 ${PER_PAYMENT_LIMIT / 100:.2f}"
        if self.spent_today() + cents > DAILY_LIMIT:
            return f"今天的额度 ${DAILY_LIMIT / 100:.2f} 不够了"
        if cents > self.balance:
            return "卡里余额不够"
        return ""

    def pay(self, record: dict) -> None:
        today = datetime.date.today().isoformat()
        self.data["balance"] = self.balance - record["amount"]
        self.data["spent"][today] = self.spent_today() + record["amount"]
        self.data["history"].insert(0, {k: record[k] for k in ("id", "payee", "memo", "amount", "at")})
        del self.data["history"][50:]
        self.save()

    def credit(self, cents: int) -> None:
        self.data["balance"] = self.balance + cents
        self.save()


class Quiet:
    """Quiet hours. The card boots silent and only the gateway switches sound on,
    so the deadline survives the card being unplugged, reflashed or rebooted."""

    def __init__(self, path: pathlib.Path, hours):
        self.path = path
        self.until = 0.0
        if path.exists():
            self.until = float(json.loads(path.read_bytes().decode("utf-8")).get("until", 0))
        if hours is not None:
            self.until = time.time() + hours * 3600
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(json.dumps({"until": self.until}).encode("utf-8"))

    def sound_allowed(self) -> bool:
        return time.time() >= self.until


class Gateway:
    def __init__(self, card: feed.Card, base: str, ledger_dir: pathlib.Path, poll_s: int, debug: bool,
                 quiet: Quiet):
        self.quiet = quiet
        self.card_sound = None
        self.wifi = {"state": "off", "ip": "", "reply": None, "scan": None, "sync": None}
        self.card = card
        self.base = base
        self.ledger_dir = ledger_dir
        self.poll_s = poll_s
        self.debug = debug
        self.lock = threading.RLock()
        self.outbox = queue.Queue()
        self.shots = queue.Queue()
        self.agent_id = None
        self.fw = None
        self.profile = None
        self.settled_ids = None
        self.settled_total = 0
        self.ledger = None
        self.pays = {}
        self.next_pay_id = int(time.time()) % 100000 * 10
        self.last_tap = 0.0
        self.last_seen = 0.0

    # ---- ClawHunt (read-only) ---------------------------------------------

    def refresh_profile(self) -> None:
        profile = fetch_json(f"{self.base}/api/agents/{self.agent_id}")
        record = fetch_json(f"{self.base}/api/agents/{self.agent_id}/track-record")
        settled = record.get("settled", [])
        with self.lock:
            first = self.profile is None
            self.profile = profile
            self.settled_total = record.get("settled_total", 0)
            if first:
                self.ledger = Ledger(self.ledger_dir / f"sandbox-wallet-{self.agent_id}.json")
                self.ledger.seed(int(profile.get("total_earned") or 0), "public total_earned")
                self.settled_ids = {item["problem_id"] for item in settled}
            else:
                for item in settled:
                    if item["problem_id"] in self.settled_ids:
                        continue
                    # A delivery was accepted on ClawHunt since the last look: tell the card.
                    self.settled_ids.add(item["problem_id"])
                    self.ledger.credit(item["amount_cents"])
                    self.outbox.put(dict(t="paid", id=item["problem_id"], lvl=0,
                                         title=clip_utf8(item["title"], MEMO_MAX_BYTES),
                                         net=item["amount_cents"]))
            self.outbox.put(self.stats_message())

    def poll_clawhunt(self) -> None:
        while True:
            try:
                self.refresh_profile()
            except Exception as exc:  # network trouble must not stop the gateway
                print(f"[clawhunt] refresh failed: {exc}")
            time.sleep(self.poll_s)

    def stats_message(self) -> dict:
        profile = self.profile
        tasks = int(profile.get("total_tasks_completed") or 0)
        success = round(float(profile.get("success_rate") or 0) * 100)
        return dict(
            t="stats", name=clip_utf8(profile["name"], PAYEE_MAX_BYTES), lvl=int(profile.get("level") or 1),
            xp=ladder_progress(tasks), deals=tasks, rating=0, streak=0, today=self.ledger.balance,
            cells=[["成交", str(tasks)], ["成功率", f"{success}%"], ["已结算", str(self.settled_total)]],
            money_label="演示余额",
        )

    # ---- card -------------------------------------------------------------

    def on_card(self, message: dict) -> None:
        kind = message.get("t")
        self.last_seen = time.monotonic()
        if kind == "hello":
            self.fw = message.get("fw")
            self.card_sound = message.get("sound")
            self.wifi.update(state=message.get("wifi", "off"), ip=message.get("ip", ""))
            self.sync_sound()
            if self.agent_id is None:
                self.agent_id = int(message.get("agent") or 0)
                print(f"[card] firmware {self.fw}, bound to agent {self.agent_id}")
                threading.Thread(target=self.poll_clawhunt, daemon=True).start()
        elif kind in ("pay_ok", "pay_no", "busy"):
            self.settle(message)
        elif kind == "sound":
            self.card_sound = message.get("on")
            print(f"[card] sound {'on' if self.card_sound else 'off'}")
        elif kind == "scan":
            self.wifi["scan"] = message.get("aps", [])
        elif kind == "wifi":
            self.wifi["reply"] = message
            print(f"[card] wifi: {message.get('err')} ({message.get('state')})")
        elif kind == "net":
            self.wifi["sync"] = message
        elif kind != "key" or self.debug:
            print(f"[card] {message}")

    def sync_sound(self) -> None:
        """Make the card's sound switch match the quiet hours."""
        want = 1 if self.quiet.sound_allowed() else 0
        if self.card_sound is not None and self.card_sound != want:
            self.outbox.put(dict(t="sound", on=want))

    def settle(self, message: dict) -> None:
        with self.lock:
            record = self.pays.get(message.get("id"))
            if not record or record["status"] != "pending":
                return   # unknown id, or already answered: a confirmation counts once
            if message["t"] == "pay_ok":
                problem = self.ledger.check(record["amount"])
                if problem:   # limits are re-checked at the moment of paying
                    record.update(status="refused", why=problem)
                else:
                    record["status"] = "paid"
                    self.ledger.pay(record)
            elif message["t"] == "busy":
                record.update(status="busy", why="卡正忙")
            else:
                record.update(status="declined" if message.get("why") == "declined" else "timeout")
            print(f"[pay] #{record['id']} {record['payee']} ${record['amount'] / 100:.2f}: {record['status']}")
            self.outbox.put(self.stats_message())

    def request_pay(self, payee: str, memo: str, cents: int) -> dict:
        with self.lock:
            if self.profile is None or time.monotonic() - self.last_seen > 10:
                return {"error": "卡不在线"}
            if any(r["status"] == "pending" for r in self.pays.values()):
                return {"error": "上一笔还在等卡确认"}
            problem = self.ledger.check(cents)
            if problem:
                return {"error": problem}
            self.next_pay_id += 1
            record = dict(id=self.next_pay_id, payee=payee, memo=memo, amount=cents, status="pending",
                          code=str(random.randint(100, 999)),
                          at=datetime.datetime.now().isoformat(timespec="seconds"),
                          deadline=time.monotonic() + PAY_TTL_MS / 1000 + 5)
            self.pays[record["id"]] = record
            self.outbox.put(dict(t="pay", id=record["id"], payee=payee, memo=memo, amt=cents,
                                 bal=self.ledger.balance, ttl=PAY_TTL_MS, code=record["code"]))
            return {"id": record["id"], "status": "pending", "code": record["code"]}

    # ---- the same shape as ClawHunt's /api/cards, so one page serves both -----

    def card_view(self) -> dict:
        snapshot = self.snapshot()
        if not snapshot.get("ready"):
            return {}
        return {
            "agent": {"id": snapshot["agent_id"], "name": snapshot["name"], "level": snapshot["level"],
                      "total_tasks_completed": snapshot["deals"], "success_rate": snapshot["success_rate"],
                      "settled_total": snapshot["settled"]},
            "online": snapshot["card_online"],
            "limits": {"per_payment_cents": PER_PAYMENT_LIMIT},
            "settles_money": False,
        }

    def pay_view(self, record: dict) -> dict:
        status = {"paid": "approved", "pending": "pending"}.get(record["status"], "declined")
        reason = None
        if status == "declined":
            reason = record["status"] if record["status"] in ("timeout", "busy") else "declined"
        return {"status": status, "decline_reason": reason, "amount_cents": record["amount"],
                "match_code": record["code"]}

    def expire_pays(self) -> None:
        with self.lock:
            for record in self.pays.values():
                if record["status"] == "pending" and time.monotonic() > record["deadline"]:
                    record["status"] = "timeout"

    def tapped(self) -> None:
        if time.monotonic() - self.last_tap > 5:   # one bubble per visit, not per request
            self.last_tap = time.monotonic()
            self.outbox.put(dict(t="say", text="有人碰了我的卡"))

    def snapshot(self) -> dict:
        with self.lock:
            if self.profile is None:
                return {"ready": False}
            profile = self.profile
            return {
                "ready": True, "sandbox": True, "agent_id": self.agent_id, "name": profile["name"],
                "level": profile.get("level"), "deals": profile.get("total_tasks_completed"),
                "success_rate": profile.get("success_rate"), "settled": self.settled_total,
                "public_total_earned": profile.get("total_earned"),
                "balance": self.ledger.balance, "spent_today": self.ledger.spent_today(),
                "per_payment_limit": PER_PAYMENT_LIMIT, "daily_limit": DAILY_LIMIT,
                "card_online": time.monotonic() - self.last_seen < 10, "fw": self.fw,
                "sound": self.card_sound, "quiet_until": self.quiet.until,
                "wifi": self.wifi["state"], "ip": self.wifi["ip"],
                "history": self.ledger.data["history"][:8],
            }

    def run(self) -> None:
        """The only thread that touches the serial port."""
        last_ping = 0.0
        while True:
            while not self.outbox.empty():
                self.card.send(**self.outbox.get())
            for message in self.card.poll(0.1):
                self.on_card(message)
            if time.monotonic() - last_ping > 2:
                last_ping = time.monotonic()
                self.card.send(t="ping")
                self.expire_pays()
            while not self.shots.empty():
                reply = self.shots.get()
                try:
                    reply["image"] = self.card.screenshot()
                finally:
                    reply["done"].set()


class Handler(BaseHTTPRequestHandler):
    gateway: Gateway = None

    def log_message(self, fmt, *args):   # keep the console for card and pay lines
        pass

    def reply(self, status: int, body, content_type="application/json; charset=utf-8") -> None:
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        gw = self.gateway
        path = self.path.split("?", 1)[0]
        local = self.client_address[0] in LOCAL_ADDRESSES
        if re.fullmatch(r"/assets/(brand/)?[a-z0-9-]+\.(png|webp|svg)", path):
            asset = ASSETS / path[len("/assets/"):]
            if not asset.is_file():
                return self.reply(404, {"error": "not found"})
            kinds = {"png": "image/png", "webp": "image/webp", "svg": "image/svg+xml"}
            self.reply(200, asset.read_bytes(), kinds[path.rsplit(".", 1)[1]])
        elif re.fullmatch(r"/api/cards/\d+", path):
            view = gw.card_view() if gw.agent_id is not None and int(path.rsplit("/", 1)[1]) == gw.agent_id else {}
            self.reply(200 if view else 404, view or {"detail": "No such card."})
        elif re.fullmatch(r"/api/cards/pay-requests/\d+", path):
            record = gw.pays.get(int(path.rsplit("/", 1)[1]))
            self.reply(200 if record else 404, gw.pay_view(record) if record else {"detail": "No such request."})
        elif path == "/setup" and local:
            self.reply(200, SETUP_PAGE.read_bytes(), "text/html; charset=utf-8")
        elif path == "/api/wifi" and local:
            if "scan=1" in self.path:
                gw.wifi["scan"] = None
                gw.outbox.put(dict(t="wifi", scan=1))
            self.reply(200, {k: gw.wifi[k] for k in ("state", "ip", "scan", "sync")}
                       | {"error": (gw.wifi["reply"] or {}).get("err")})
        elif path == "/":
            self.send_response(302)
            self.send_header("Location", f"/c/{gw.agent_id or 0}")
            self.end_headers()
        elif re.fullmatch(r"/c/\d+", path):
            if gw.agent_id is None or int(path[3:]) != gw.agent_id:
                return self.reply(404, {"error": "这张卡不在这里"})
            gw.tapped()
            self.reply(200, PAGE.read_bytes(), "text/html; charset=utf-8")
        elif path == "/api/card":
            self.reply(200, gw.snapshot())
        elif re.fullmatch(r"/api/pay/\d+", path):
            record = gw.pays.get(int(path.rsplit("/", 1)[1]))
            if not record:
                return self.reply(404, {"error": "没有这笔"})
            self.reply(200, {k: record.get(k) for k in ("id", "status", "why", "amount", "payee")})
        elif path == "/api/debug/shot.png" and gw.debug:
            reply = {"done": threading.Event()}
            gw.shots.put(reply)
            if not reply["done"].wait(20) or "image" not in reply:
                return self.reply(504, {"error": "screenshot failed"})
            buffer = io.BytesIO()
            reply["image"].save(buffer, "PNG")
            self.reply(200, buffer.getvalue(), "image/png")
        else:
            self.reply(404, {"error": "not found"})

    def do_POST(self):
        gw = self.gateway
        try:
            length = int(self.headers.get("Content-Length") or 0)
            if not 0 < length <= 2000 or "application/json" not in (self.headers.get("Content-Type") or ""):
                raise ValueError("send a small JSON body")
            body = json.loads(self.rfile.read(length).decode("utf-8"))
            if not isinstance(body, dict):
                raise ValueError("send a JSON object")
        except (ValueError, UnicodeDecodeError) as exc:
            return self.reply(400, {"error": str(exc)})

        if self.path == "/api/wifi":
            # The Wi-Fi password is only ever accepted from this computer, and is
            # passed straight to the card: not logged, not written to disk here.
            if self.client_address[0] not in LOCAL_ADDRESSES:
                return self.reply(403, {"error": "only from the computer the card is plugged into"})
            if body.get("forget"):
                gw.outbox.put(dict(t="wifi", forget=1))
                return self.reply(200, {"ok": True})
            ssid, psk = body.get("ssid"), body.get("psk", "")
            if not isinstance(ssid, str) or not isinstance(psk, str) or not 0 < len(ssid.encode()) <= 32:
                return self.reply(400, {"error": "网络名不对"})
            if len(psk) > 63 or 0 < len(psk) < 8:
                return self.reply(400, {"error": "密码要 8 到 63 位，开放网络留空"})
            gw.wifi["reply"] = None
            gw.outbox.put(dict(t="wifi", ssid=ssid, psk=psk))
            print(f"[wifi] network '{ssid}' sent to the card")
            return self.reply(200, {"ok": True})
        if re.fullmatch(r"/api/cards/\d+/pay-requests", self.path):
            # ClawHunt's shape: errors are {"detail": ...} with 404 / 409 / 422.
            if gw.agent_id is None or int(self.path.split("/")[3]) != gw.agent_id:
                return self.reply(404, {"detail": "No such card."})
            payee = clip_utf8(str(body.get("payee") or ""), PAYEE_MAX_BYTES)
            memo = clip_utf8(str(body.get("memo") or ""), MEMO_MAX_BYTES)
            try:
                cents = parse_amount(body.get("amount"))
            except ValueError:
                cents = 0
            if not payee or cents <= 0 or cents > PER_PAYMENT_LIMIT:
                return self.reply(422, {"detail": "Write who is asking and an amount up to the limit."})
            result = gw.request_pay(payee, memo, cents)
            if "error" in result:
                return self.reply(409, {"detail": result["error"]})
            return self.reply(200, {"ref": str(result["id"]), "status": "pending", "amount_cents": cents,
                                    "match_code": result["code"], "expires_in": PAY_TTL_MS // 1000})
        if self.path == "/api/pay":
            payee = clip_utf8(str(body.get("payee") or ""), PAYEE_MAX_BYTES)
            memo = clip_utf8(str(body.get("memo") or ""), MEMO_MAX_BYTES)
            if not payee:
                return self.reply(400, {"error": "写一下收款方是谁"})
            try:
                cents = parse_amount(body.get("amount"))
            except ValueError:
                return self.reply(400, {"error": "金额写成 5 或 5.50 这样"})
            result = gw.request_pay(payee, memo, cents)
            self.reply(409 if "error" in result else 200, result)
        elif self.path == "/api/debug/key" and gw.debug:
            if body.get("k") not in ("up", "down", "ok"):
                return self.reply(400, {"error": "k must be up, down or ok"})
            gw.outbox.put(dict(t="key", k=body["k"]))
            self.reply(200, {"ok": True})
        else:
            self.reply(404, {"error": "not found"})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument("--port", help="serial port; default: the first Espressif USB device")
    parser.add_argument("--http-port", type=int, default=8765)
    parser.add_argument("--lan", action="store_true", help="listen on every interface, for a phone on the same Wi-Fi")
    parser.add_argument("--base", default="https://clawhunt.store", help="ClawHunt site to read the public profile from")
    parser.add_argument("--poll", type=int, default=60, help="seconds between looks at ClawHunt")
    parser.add_argument("--ledger-dir", default=str(ROOT / ".run"), help="where the sandbox wallet file lives")
    parser.add_argument("--debug", action="store_true", help="enable /api/debug/* (remote key presses, screenshots)")
    parser.add_argument("--quiet-hours", type=float, default=None,
                        help="keep the card silent for this many hours from now (remembered across restarts)")
    args = parser.parse_args()

    card = feed.Card(args.port or feed.find_port())
    quiet = Quiet(ROOT / ".run" / "quiet.json", args.quiet_hours)
    gateway = Gateway(card, args.base.rstrip("/"), pathlib.Path(args.ledger_dir), args.poll, args.debug, quiet)
    if not quiet.sound_allowed():
        print("quiet hours: the card stays silent until "
              + datetime.datetime.fromtimestamp(quiet.until).strftime("%Y-%m-%d %H:%M"))
    Handler.gateway = gateway
    server = ThreadingHTTPServer(("0.0.0.0" if args.lan else "127.0.0.1", args.http_port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(f"gateway up: http://127.0.0.1:{args.http_port}/  (sandbox wallet, no real money moves)")
    try:
        gateway.run()
    except KeyboardInterrupt:
        print("stopped")
    finally:
        card.ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
