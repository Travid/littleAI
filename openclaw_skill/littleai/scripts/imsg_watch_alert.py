#!/usr/bin/env python3
"""Watch iMessage chats and alert via littleAI (NO outbound texting).

This is meant to run as a long-lived background process on macOS.
It polls `imsg` for new messages in specific chats and, when a message looks
important, it triggers a littleAI attention animation (caption + blink + beep;
optionally speaks for urgent messages).

IMPORTANT:
- This script does NOT send iMessages.
- Provide watch targets via --watch args or env vars (don't hardcode personal numbers in public repos).

Example:
  bash openclaw_skill/littleai/scripts/ensure_venv.sh
  source openclaw_skill/littleai/.venv/bin/activate
  python3 openclaw_skill/littleai/scripts/imsg_watch_alert.py \
    --littleai-ip 192.168.1.158 \
    --watch "+15551234567:Mom" \
    --watch "+15557654321:Husband" \
    --poll-seconds 20 \
    --speak-urgent

State:
- lock:  ~/.openclaw/state/littleai-imsg-watch.lock
- state: ~/.openclaw/state/littleai-imsg-watch-state.json
Logs: redirect stdout/stderr when running in background.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

# Only needed for the lock
import fcntl


@dataclass
class WatchTarget:
    handle: str
    label: str


URGENT_KEYWORDS = {
    "urgent",
    "asap",
    "emergency",
    "911",
    "hospital",
    "call",
    "call me",
    "now",
    "right now",
    "immediately",
    "help",
}

IMPORTANT_KEYWORDS = {
    "today",
    "tonight",
    "tomorrow",
    "soon",
    "when",
    "where",
    "can you",
    "could you",
    "please",
    "need",
    "issue",
    "problem",
    "stuck",
    "question",
}

TRIVIAL = {"k", "ok", "okay", "lol", "thanks", "thx", "👍", "👌"}


def sh(cmd: List[str], timeout_s: int = 10) -> str:
    return subprocess.check_output(cmd, text=True, timeout=timeout_s)


def norm_handle(h: str) -> str:
    # Keep + and digits only for comparisons
    h = h.strip()
    if h.startswith("+"):
        return "+" + re.sub(r"\D", "", h)
    return re.sub(r"\D", "", h)


def parse_jsonl(s: str) -> List[dict]:
    out = []
    for line in s.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            # tolerate weird output
            continue
    return out


def find_chat_id_for_handle(handle: str) -> Optional[int]:
    # imsg chats returns JSONL entries like: {id, identifier, ...}
    entries = parse_jsonl(sh(["imsg", "chats", "--limit", "200", "--json"], timeout_s=20))
    want = norm_handle(handle)
    for e in entries:
        ident = str(e.get("identifier", ""))
        if norm_handle(ident) == want:
            cid = e.get("id")
            if isinstance(cid, int):
                return cid
    return None


def load_state(path: str) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return {}
    except Exception:
        return {}


def save_state(path: str, state: dict) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2, sort_keys=True)
    os.replace(tmp, path)


def looks_important(text: str) -> Tuple[bool, bool]:
    """Return (important, urgent)."""
    t = (text or "").strip()
    if not t:
        return (False, False)

    tl = t.lower()

    if tl in TRIVIAL and len(tl) <= 5:
        return (False, False)

    urgent = any(k in tl for k in URGENT_KEYWORDS)
    important = urgent

    if "?" in t:
        important = True

    if re.search(r"\b\d{1,2}:\d{2}\b", tl) or re.search(r"\b\d{1,2}\s?(am|pm)\b", tl):
        important = True

    if any(k in tl for k in IMPORTANT_KEYWORDS):
        important = True

    if len(t) >= 25:
        important = True

    return (important, urgent)


def truncate_one_line(s: str, n: int = 72) -> str:
    s = re.sub(r"\s+", " ", (s or "").strip())
    if len(s) <= n:
        return s
    return s[: n - 1] + "…"


def alert_littleai(littleai_ip: str, label: str, text: str, urgent: bool, speak_urgent: bool) -> None:
    # Delegate to the existing attention script (handles caption + blink + optional beep/speak).
    mode = "urgent" if urgent else "normal"

    caption = f"{label}: {truncate_one_line(text)}"

    cmd = [
        sys.executable,
        os.path.join(os.path.dirname(__file__), "attention.py"),
        "--ip",
        littleai_ip,
        "--mode",
        mode,
        "--text",
        caption,
        "--beep",
    ]
    if urgent and speak_urgent:
        cmd.append("--speak")

    subprocess.run(cmd, check=False, timeout=60)


def acquire_lock(lock_path: str):
    os.makedirs(os.path.dirname(lock_path), exist_ok=True)
    f = open(lock_path, "w", encoding="utf-8")
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        # already running
        raise SystemExit(f"Already running (lock busy): {lock_path}")
    return f


def bootstrap_last_seen(chat_id: int) -> int:
    # Seed last_seen to most recent message id so we don't alert on old messages at startup.
    entries = parse_jsonl(sh(["imsg", "history", "--chat-id", str(chat_id), "--limit", "1", "--json"], timeout_s=20))
    if not entries:
        return 0
    mid = entries[0].get("id")
    return int(mid) if isinstance(mid, int) or (isinstance(mid, str) and mid.isdigit()) else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--littleai-ip", default=os.environ.get("LITTLEAI_IP"), help="littleAI device IP")
    ap.add_argument(
        "--watch",
        action="append",
        default=[],
        help='Watch target in the form "+15551234567:Label" (repeatable).',
    )
    ap.add_argument("--poll-seconds", type=int, default=20)
    ap.add_argument("--speak-urgent", action="store_true", help="Speak the caption for urgent messages")
    args = ap.parse_args()

    if not args.littleai_ip:
        raise SystemExit("Missing --littleai-ip (or set LITTLEAI_IP).")

    targets: List[WatchTarget] = []
    for w in args.watch:
        if ":" not in w:
            raise SystemExit(f"Bad --watch value (expected handle:label): {w}")
        handle, label = w.split(":", 1)
        targets.append(WatchTarget(handle=handle.strip(), label=label.strip() or handle.strip()))

    if not targets:
        raise SystemExit("No watch targets. Provide --watch +1555...:Label (repeatable).")

    state_dir = os.path.expanduser("~/.openclaw/state")
    lock_path = os.path.join(state_dir, "littleai-imsg-watch.lock")
    state_path = os.path.join(state_dir, "littleai-imsg-watch-state.json")

    _lock_file = acquire_lock(lock_path)

    state = load_state(state_path)
    if "last_seen" not in state:
        state["last_seen"] = {}

    # Resolve chat ids (retry later if missing)
    resolved: Dict[str, int] = {}

    print("[imsg-watch] starting")
    print(f"[imsg-watch] littleai_ip={args.littleai_ip} poll={args.poll_seconds}s")

    while True:
        try:
            for t in targets:
                key = norm_handle(t.handle)

                if key not in resolved:
                    cid = find_chat_id_for_handle(t.handle)
                    if cid is None:
                        print(f"[imsg-watch] chat not found for {t.handle} ({t.label}); will retry")
                        continue
                    resolved[key] = cid
                    # seed last_seen if missing
                    if key not in state["last_seen"]:
                        state["last_seen"][key] = bootstrap_last_seen(cid)
                        save_state(state_path, state)
                        print(f"[imsg-watch] seeded last_seen for {t.label} chat_id={cid} -> {state['last_seen'][key]}")

                cid = resolved[key]
                last_seen = int(state["last_seen"].get(key, 0) or 0)

                # Fetch a few latest messages and look for new inbound messages.
                entries = parse_jsonl(
                    sh(["imsg", "history", "--chat-id", str(cid), "--limit", "10", "--json"], timeout_s=20)
                )
                # entries are newest-first
                new = [e for e in entries if int(e.get("id", 0) or 0) > last_seen]
                if not new:
                    continue

                # Update last_seen to highest id we saw, regardless of from_me, so we don't reprocess.
                max_id = max(int(e.get("id", 0) or 0) for e in new)
                state["last_seen"][key] = max_id
                save_state(state_path, state)

                # Find newest inbound message among the new ones.
                inbound = [e for e in new if not bool(e.get("is_from_me", False))]
                if not inbound:
                    continue

                newest = max(inbound, key=lambda e: int(e.get("id", 0) or 0))
                text = str(newest.get("text", "") or "")

                important, urgent = looks_important(text)
                if not important:
                    continue

                print(f"[imsg-watch] alert: {t.label} urgent={urgent} text={truncate_one_line(text, 120)}")
                alert_littleai(args.littleai_ip, t.label, text, urgent=urgent, speak_urgent=args.speak_urgent)

            time.sleep(max(5, int(args.poll_seconds)))

        except KeyboardInterrupt:
            print("[imsg-watch] stopping")
            return
        except Exception as e:
            print(f"[imsg-watch] error: {e}")
            time.sleep(5)


if __name__ == "__main__":
    main()
