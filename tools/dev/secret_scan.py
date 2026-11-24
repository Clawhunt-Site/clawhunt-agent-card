"""Look for secrets in files before they go into a repository. Prints where and what KIND, never the value: a
value is shown as its first two characters and its length.

    python -I -X utf8 secret_scan.py <root> [<root> ...] [--skip name,name] [--max-bytes 2000000]

What it looks for:
- key material and tokens by their shape: private-key blocks, AWS access keys, GitHub / GitLab / Slack / OpenAI-style
  tokens, JSON web tokens, URLs that carry a user and password;
- a value given to a name that says it is a secret (password, passwd, pwd, psk, secret, token, api key, authorization,
  bearer, credential, cookie, session), in code, JSON, YAML, INI, shell and Markdown; placeholders and names of
  other variables are not reported;
- Wi-Fi network names next to passwords;
- long random-looking strings (40 or more characters of one base64 / hex run) wherever they stand.
Binary files are read for the token shapes only. Directories named in --skip are not entered (node_modules, .git,
build and the like are skipped always).
"""
import math
import os
import re
import sys

ALWAYS_SKIP = {"node_modules", ".git", "build", "managed_components", "__pycache__", ".venv", "venv", "dist"}
SHAPES = [
    ("private key block", re.compile(rb"-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("AWS access key id", re.compile(rb"\b(?:AKIA|ASIA)[0-9A-Z]{16}\b")),
    ("GitHub token", re.compile(rb"\bgh[pousr]_[A-Za-z0-9]{30,}\b")),
    ("GitHub fine-grained token", re.compile(rb"\bgithub_pat_[A-Za-z0-9_]{40,}\b")),
    ("GitLab token", re.compile(rb"\bglpat-[A-Za-z0-9_\-]{20,}\b")),
    ("Slack token", re.compile(rb"\bxox[baprs]-[A-Za-z0-9\-]{10,}\b")),
    ("sk- style API key", re.compile(rb"\bsk-[A-Za-z0-9_\-]{20,}\b")),
    ("JSON web token", re.compile(rb"\beyJ[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\b")),
    ("URL with credentials", re.compile(rb"\b[a-z][a-z0-9+.\-]*://[^\s/:@\"']{1,64}:[^\s/@\"']{3,}@[A-Za-z0-9.\-]+")),
    ("Google API key", re.compile(rb"\bAIza[0-9A-Za-z_\-]{35}\b")),
]
NAME = r"(?:pass(?:word|wd|phrase)?|pwd|psk|secret|token|api[_\-]?key|apikey|access[_\-]?key|auth(?:orization)?|bearer|credential|cookie|session[_\-]?(?:id|key)|private[_\-]?key|wifi[_\-]?(?:pass|pwd|key))"
ASSIGN = re.compile(
    r"""(?ix)
    (?P<name>[A-Za-z0-9_.\-\[\]"']*""" + NAME + r"""[A-Za-z0-9_.\-\[\]"']*)
    \s*(?:[:=]|=>|\s-\s|：)\s*
    (?P<q>["'`]?)(?P<val>[^"'`\s,;)}\]]{4,})(?P=q)
    """
)
PLACEHOLDER = re.compile(
    r"(?i)^(?:x{3,}|\*{3,}|\.{3,}|<[^>]*>|\$\{?[A-Za-z_][A-Za-z0-9_]*\}?|%[A-Za-z_]+%|your[_\-].*|changeme|example.*|none|null|true|false|undefined|"
    r"os\.environ.*|process\.env.*|getenv.*|args?\..*|self\..*|str|string|bytes|int|bool|todo|redacted|\(redacted\)|n/?a|unset|empty|"
    r"[A-Za-z_][A-Za-z0-9_]*\(.*|[A-Za-z_][A-Za-z0-9_.]*\[.*)$"
)
RANDOM = re.compile(rb"(?<![A-Za-z0-9+/=_\-])[A-Za-z0-9+/_\-]{40,}={0,2}(?![A-Za-z0-9+/=_\-])")
TEXT_EXT = {".py", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".json", ".md", ".txt", ".yml", ".yaml", ".ini", ".cfg", ".conf", ".toml", ".sh", ".ps1", ".bat", ".cmd",
            ".c", ".h", ".cpp", ".hpp", ".cmake", ".env", ".log", ".html", ".css", ".csv", ".xml", ".defaults", ".lock", ""}


def mask(v):
    v = v if isinstance(v, str) else v.decode("utf-8", "replace")
    return f"{v[:2]}…({len(v)} chars)"


def entropy(b):
    if not b:
        return 0.0
    counts = {}
    for c in b:
        counts[c] = counts.get(c, 0) + 1
    return -sum(n / len(b) * math.log2(n / len(b)) for n in counts.values())


def looks_like_identifier(v):
    """A value that is a plain word, a path, a number or the name of another variable is not a secret."""
    if re.fullmatch(r"[0-9.]+", v):
        return True
    if re.fullmatch(r"[A-Za-z_]+", v) and len(v) < 24:
        return True
    if re.fullmatch(r"[A-Z0-9_]+", v):
        return True
    if "/" in v and not re.search(r"[A-Za-z0-9]{24,}", v):
        return True
    return False


def scan_file(path, max_bytes):
    out = []
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as fh:
            data = fh.read(max_bytes)
    except OSError as e:
        return [("unreadable", 0, str(e))]
    for kind, rx in SHAPES:
        for m in rx.finditer(data):
            line = data.count(b"\n", 0, m.start()) + 1
            out.append((kind, line, mask(m.group(0))))
    ext = os.path.splitext(path)[1].lower()
    is_text = ext in TEXT_EXT and b"\x00" not in data[:4096]
    if not is_text:
        return out
    text = data.decode("utf-8", "replace")
    for i, ln in enumerate(text.splitlines(), 1):
        if len(ln) > 2000:
            ln = ln[:2000]
        for m in ASSIGN.finditer(ln):
            val = m.group("val")
            if PLACEHOLDER.match(val) or looks_like_identifier(val):
                continue
            # a value worth reporting: has a digit or a symbol or is long, i.e. not an ordinary word
            if len(val) >= 8 and (re.search(r"[0-9]", val) or re.search(r"[^A-Za-z0-9]", val) or len(val) >= 20):
                out.append((f"value given to `{m.group('name')[:40]}`", i, mask(val)))
        if re.search(r"(?i)\bssid\b", ln) and re.search(r"(?i)pass|psk|pwd|key", ln):
            out.append(("Wi-Fi name and password on one line", i, "(line not shown)"))
    for m in RANDOM.finditer(data):
        s = m.group(0)
        # a checksum (40 or 64 hex characters) is not a secret; a path or a sentence of words is not random
        if re.fullmatch(rb"[0-9a-fA-F]{40}|[0-9a-fA-F]{64}", s) or s.count(b"/") >= 3 or s.count(b"-") + s.count(b"_") >= 5:
            continue
        if entropy(s) >= 4.5 and re.search(rb"[0-9]", s) and re.search(rb"[A-Za-z]", s):
            line = data.count(b"\n", 0, m.start()) + 1
            out.append(("long random-looking string", line, mask(s)))
    return out


def main():
    args = sys.argv[1:]
    skip = set(ALWAYS_SKIP)
    max_bytes = 2_000_000
    if "--skip" in args:
        i = args.index("--skip")
        skip |= set(args[i + 1].split(","))
        del args[i:i + 2]
    if "--max-bytes" in args:
        i = args.index("--max-bytes")
        max_bytes = int(args[i + 1])
        del args[i:i + 2]
    files = 0
    hits = 0
    by_kind = {}
    for root in args:
        if os.path.isfile(root):
            walk = [(os.path.dirname(root), [], [os.path.basename(root)])]
        else:
            walk = os.walk(root)
        for dp, dn, fn in walk:
            dn[:] = [d for d in dn if d not in skip]
            for n in fn:
                p = os.path.join(dp, n)
                files += 1
                found = scan_file(p, max_bytes)
                # one line per file and kind, with the first few places
                kinds = {}
                for kind, line, shown in found:
                    kinds.setdefault(kind, []).append((line, shown))
                for kind, places in kinds.items():
                    hits += len(places)
                    by_kind[kind.split("`")[0]] = by_kind.get(kind.split("`")[0], 0) + len(places)
                    where = ", ".join(f"line {l}: {s}" for l, s in places[:4]) + ("" if len(places) <= 4 else f", and {len(places) - 4} more")
                    print(f"{p.replace(os.sep, '/')}: {kind}: {where}")
    print(f"\n{files} files read, {hits} places reported")
    for k, v in sorted(by_kind.items(), key=lambda kv: -kv[1]):
        print(f"  {v:5d}  {k}")


if __name__ == "__main__":
    main()
