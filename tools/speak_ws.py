#!/usr/bin/env python3
"""Generate TTS on macOS using `say` and stream PCM chunks to the device over WebSocket.

Usage:
  python3 tools/speak_ws.py --ip 192.168.1.158 --text "Hello Dave"

Notes:
- Device expects WS: ws://<ip>:8080/ws
- Command: {"type":"speak_pcm","data_b64":"..."}
- Audio format: PCM16 little-endian, mono, 16000 Hz.
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
    """Return a smoothed mouth_open value 0..1 based on RMS amplitude."""
    a = array.array('h')
    a.frombytes(frames)
    if not a:
        return prev
    # RMS
    acc = 0
    for s in a:
        acc += s * s
    rms = (acc / len(a)) ** 0.5
    # Map RMS -> openness (tune this empirically)
    target = min(1.0, max(0.0, rms / 7000.0))
    # Smooth
    return prev * 0.70 + target * 0.30


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
    # macOS `say` can emit WAV at a specified format.
    # If this fails on your macOS version, we can fall back to AIFF+afconvert.
    cmd = ["say", "-o", out_wav, "--data-format=LEI16@16000", sanitize_text(text)]
    subprocess.check_call(cmd)


async def stream_wav(ip: str, wav_path: str, drive_face: bool = True) -> None:
    uri = f"ws://{ip}:8080/ws"
    async with websockets.connect(uri, max_size=2**20) as ws:
        if drive_face:
            # Sticky rig control while speaking (mouth only). Let expression drive eyes.
            await ws.send('{"type":"mouth","open":0.00}')
            await ws.recv()

        mouth_prev = 0.0

        with wave.open(wav_path, "rb") as w:
            assert w.getnchannels() == 1, w.getnchannels()
            assert w.getsampwidth() == 2, w.getsampwidth()
            assert w.getframerate() == SAMPLE_RATE, w.getframerate()

            while True:
                frames = w.readframes(CHUNK_SAMPLES)
                if not frames:
                    break

                if drive_face:
                    mouth_prev = rms_open(frames, mouth_prev)
                    await ws.send('{"type":"mouth","open":' + f"{mouth_prev:.3f}" + '}')
                    await ws.recv()

                b64 = base64.b64encode(frames).decode("ascii")
                await ws.send('{"type":"speak_pcm","data_b64":"' + b64 + '"}')
                rep = await ws.recv()
                if '"ok":true' not in rep:
                    print(rep)

        if drive_face:
            await ws.send('{"type":"mouth","open":0.00}')
            await ws.recv()
            await ws.send('{"type":"rig_clear"}')
            await ws.recv()

        # quick end-beep (optional)
        await ws.send('{"type":"beep","freq_hz":660,"duration_ms":120}')
        print(await ws.recv())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--text", required=True)
    ap.add_argument("--no-face", action="store_true", help="Don't animate face rig while speaking")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as td:
        wav_path = os.path.join(td, "tts.wav")
        gen_wav_say(args.text, wav_path)
        asyncio.run(stream_wav(args.ip, wav_path, drive_face=(not args.no_face)))


if __name__ == "__main__":
    main()
