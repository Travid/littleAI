#!/usr/bin/env python3
"""Generate TTS on macOS using `say` and stream PCM chunks to littleAI over WebSocket.

Usage:
  python3 {baseDir}/scripts/speak_ws.py --ip DEVICE_IP --text "Hello Dave"

Notes:
- WS: ws://<ip>:8080/ws
- Command: {"type":"speak_pcm","data_b64":"..."}
- Audio format: PCM16 little-endian, mono, 16000 Hz.
- Animates mouth openness from RMS by default. Use --no-face to disable.
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


async def stream_wav(ip: str, wav_path: str, drive_face: bool = True) -> None:
    uri = f"ws://{ip}:8080/ws"
    async with websockets.connect(uri, max_size=2**20) as ws:
        if drive_face:
            await ws.send('{"type":"mouth","open":0.00}')
            await ws.recv()

        mouth_prev = 0.0

        with wave.open(wav_path, "rb") as w:
            assert w.getnchannels() == 1
            assert w.getsampwidth() == 2
            assert w.getframerate() == SAMPLE_RATE

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

        await ws.send('{"type":"beep","freq_hz":660,"duration_ms":120}')
        await ws.recv()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", required=True)
    ap.add_argument("--text", required=True)
    ap.add_argument("--no-face", action="store_true")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as td:
        wav_path = os.path.join(td, "tts.wav")
        gen_wav_say(args.text, wav_path)
        asyncio.run(stream_wav(args.ip, wav_path, drive_face=(not args.no_face)))


if __name__ == "__main__":
    main()
