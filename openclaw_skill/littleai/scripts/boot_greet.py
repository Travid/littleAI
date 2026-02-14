#!/usr/bin/env python3
"""littleAI gateway-boot greeting (yawn + short hello).

Intended to be called automatically from OpenClaw's BOOT.md via the boot-md hook.

Example:
  source openclaw_skill/littleai/.venv/bin/activate
  python3 openclaw_skill/littleai/scripts/boot_greet.py --ip DEVICE_IP

Notes:
- Uses macOS `say` to generate a short TTS clip (PCM16@16k) and streams it over WS.
- Picks a greeting phrase randomly so it varies a bit.
"""

import argparse
import asyncio
import base64
import os
import random
import subprocess
import tempfile
import wave
import array

import websockets

SAMPLE_RATE = 16000
CHUNK_SAMPLES = 800  # 50ms @ 16kHz

GREETINGS = [
    "Howdy!",
    "Hey there!",
    "Hello hello!",
    "Hi!",
    "Morning!",
    "Good to see you!",
    "What's up?",
]


def sanitize_text(s: str) -> str:
    # Keep captions/inputs ASCII-ish so LVGL doesn't show missing-glyph squares.
    return (
        s.replace("’", "'")
        .replace("‘", "'")
        .replace("“", '"')
        .replace("”", '"')
        .replace("…", "...")
        .replace("—", "-")
        .replace("–", "-")
        .replace("\u00A0", " ")
    )


def gen_wav_say(text: str, out_wav: str) -> None:
    cmd = ["say", "-o", out_wav, "--data-format=LEI16@16000", sanitize_text(text)]
    subprocess.check_call(cmd)


def rms_open(frames: bytes, prev: float) -> float:
    a = array.array("h")
    a.frombytes(frames)
    if not a:
        return prev
    acc = 0
    for s in a:
        acc += s * s
    rms = (acc / len(a)) ** 0.5
    target = min(1.0, max(0.0, rms / 7000.0))
    return prev * 0.70 + target * 0.30


async def ws_send(ws, obj):
    await ws.send(__import__("json").dumps(obj))
    return await ws.recv()


async def yawn(ws):
    # A simple yawn animation: sleepy expression + slow blink + mouth opens wide then closes.
    await ws_send(ws, {"type": "set_expression", "expression": "sleeping"})
    await ws_send(ws, {"type": "gaze", "x": 0.0, "y": 0.15})

    # Squint a bit.
    await ws_send(ws, {"type": "eyes", "open": 0.25})
    await asyncio.sleep(0.15)

    # Start yawn: open mouth.
    await ws_send(ws, {"type": "mouth", "open": 0.10})
    await ws_send(ws, {"type": "blink", "duration_ms": 450})
    await asyncio.sleep(0.20)

    await ws_send(ws, {"type": "mouth", "open": 1.00})
    await asyncio.sleep(0.55)

    # Close mouth smoothly.
    steps = 10
    for i in range(steps):
        v = 1.0 - ((i + 1) / steps) * 0.95
        await ws_send(ws, {"type": "mouth", "open": float(f"{v:.3f}")})
        await asyncio.sleep(0.06)

    await ws_send(ws, {"type": "mouth", "open": 0.0})
    await ws_send(ws, {"type": "rig_clear"})


async def stream_wav(ws, wav_path: str):
    # Drive mouth openness from audio energy while streaming.
    await ws_send(ws, {"type": "mouth", "open": 0.0})

    mouth_prev = 0.0
    with wave.open(wav_path, "rb") as w:
        assert w.getnchannels() == 1
        assert w.getsampwidth() == 2
        assert w.getframerate() == SAMPLE_RATE

        while True:
            frames = w.readframes(CHUNK_SAMPLES)
            if not frames:
                break

            mouth_prev = rms_open(frames, mouth_prev)
            await ws_send(ws, {"type": "mouth", "open": float(f"{mouth_prev:.3f}")})

            b64 = base64.b64encode(frames).decode("ascii")
            await ws.send('{"type":"speak_pcm","data_b64":"' + b64 + '"}')
            rep = await ws.recv()
            if '"ok":true' not in rep:
                print(rep)

    await ws_send(ws, {"type": "mouth", "open": 0.0})
    await ws_send(ws, {"type": "rig_clear"})


async def run(ip: str):
    greeting = random.choice(GREETINGS)

    uri = f"ws://{ip}:8080/ws"
    async with websockets.connect(uri, max_size=2**20) as ws:
        # caption first, so you see something immediately
        await ws_send(ws, {"type": "caption", "text": sanitize_text(greeting), "ttl_ms": 8000})

        # yawn
        await yawn(ws)

        # perk up a bit
        await ws_send(ws, {"type": "set_expression", "expression": random.choice(["neutral", "happy", "surprised"])})
        await ws_send(ws, {"type": "gaze", "x": random.choice([-0.15, 0.0, 0.18]), "y": -0.05})

        # speak
        with tempfile.TemporaryDirectory() as td:
            wav_path = os.path.join(td, "tts.wav")
            gen_wav_say(greeting, wav_path)
            await stream_wav(ws, wav_path)

        # settle
        await asyncio.sleep(0.2)
        await ws_send(ws, {"type": "set_expression", "expression": "neutral"})
        await ws_send(ws, {"type": "gaze", "x": 0.0, "y": 0.0})
        await ws_send(ws, {"type": "rig_clear"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", help="Device IP (or set LITTLEAI_IP env var)")
    args = ap.parse_args()

    ip = args.ip or os.environ.get("LITTLEAI_IP")
    if not ip:
        raise SystemExit("Missing --ip (or set LITTLEAI_IP).")

    asyncio.run(run(ip))


if __name__ == "__main__":
    main()
