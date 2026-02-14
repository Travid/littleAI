#!/usr/bin/env python3
"""Get Dave's attention via littleAI over WebSocket.

This script drives BOTH the face and audio:
- caption + expression + blink
- optional TTS streamed as PCM16@16k (generated on macOS via `say`)
- optional beep pattern

Examples:
  source .venv-ws/bin/activate
  python3 tools/attention.py --ip 192.168.1.158 --text "Dinner is ready" --speak

  python3 tools/attention.py --ip 192.168.1.158 --text "Check email" --mode urgent --beep --speak

Modes:
  gentle: 1 beep + small blink + happy
  normal: 2 beeps + blink + surprised
  urgent: 3 beeps + repeated blinks + angry
"""

import argparse
import asyncio
import base64
import os
import subprocess
import tempfile
import wave
import array

import websockets

SAMPLE_RATE = 16000
CHUNK_SAMPLES = 800  # 50ms @ 16kHz


def rms_open(frames: bytes, prev: float) -> float:
    a = array.array('h')
    a.frombytes(frames)
    if not a:
        return prev
    acc = 0
    for s in a:
        acc += s * s
    rms = (acc / len(a)) ** 0.5
    target = min(1.0, max(0.0, rms / 7000.0))
    return prev * 0.70 + target * 0.30


def sanitize_text(s: str) -> str:
    # LVGL's default Montserrat font set often doesn't include smart quotes, ellipsis, em-dash, etc.
    # Replace with ASCII so captions don't show as missing-glyph squares.
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
    # macOS `say` can emit WAV at a specified format.
    cmd = ["say", "-o", out_wav, "--data-format=LEI16@16000", sanitize_text(text)]
    subprocess.check_call(cmd)


async def ws_send(ws, obj):
    await ws.send(__import__("json").dumps(obj))
    return await ws.recv()


async def stream_wav(ws, wav_path: str):
    # Drive mouth openness from audio energy while streaming (mouth only).
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


async def run(ip: str, text: str, mode: str, beep: bool, speak: bool):
    uri = f"ws://{ip}:8080/ws"
    async with websockets.connect(uri, max_size=2**20) as ws:
        # face behavior
        if mode == "gentle":
            expr = "happy"
            blink_ms = 200
            beeps = [(880, 120)]
        elif mode == "urgent":
            expr = "angry"
            blink_ms = 350
            beeps = [(880, 140), (880, 140), (660, 200)]
        else:
            expr = "surprised"
            blink_ms = 250
            beeps = [(880, 140), (1320, 140)]

        # Show caption immediately (sanitized for LVGL font coverage)
        await ws_send(ws, {"type": "caption", "text": sanitize_text(text), "ttl_ms": 12000})
        await ws_send(ws, {"type": "set_expression", "expression": expr})

        # A little eye movement to make it feel alive
        if mode == "urgent":
            await ws_send(ws, {"type": "gaze", "x": -0.55, "y": -0.05})
        else:
            await ws_send(ws, {"type": "gaze", "x": 0.35, "y": -0.1})
        await ws_send(ws, {"type": "blink", "duration_ms": blink_ms})

        # Beep pattern (optional)
        if beep:
            for f, d in beeps:
                await ws_send(ws, {"type": "beep", "freq_hz": f, "duration_ms": d})
                await asyncio.sleep(0.05)

        # Speak (optional)
        if speak:
            with tempfile.TemporaryDirectory() as td:
                wav_path = os.path.join(td, "tts.wav")
                gen_wav_say(text, wav_path)
                await stream_wav(ws, wav_path)

        # Reset expression back to neutral after a beat
        await asyncio.sleep(0.4)
        await ws_send(ws, {"type": "set_expression", "expression": "neutral"})
        await ws_send(ws, {"type": "gaze", "x": 0.0, "y": 0.0})
        # Ensure rig overrides aren't left sticky if we didn't speak
        await ws_send(ws, {"type": "rig_clear"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--text", required=True)
    ap.add_argument("--mode", choices=["gentle", "normal", "urgent"], default="normal")
    ap.add_argument("--beep", action="store_true", help="Play a beep pattern")
    ap.add_argument("--speak", action="store_true", help="Speak text via macOS say -> streamed PCM")
    args = ap.parse_args()

    asyncio.run(run(args.ip, args.text, args.mode, args.beep, args.speak))


if __name__ == "__main__":
    main()
