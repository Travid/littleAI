---
name: littleai
description: Control the littleAI ESP32-S3 face+speaker device over its WebSocket control plane (expressions, gaze, blink, caption, visemes, parametric eye/mouth openness rig, and streaming TTS audio). Use when the user asks to make littleAI speak, get their attention, change the face, test WS commands, or automate attention/speech/animations.
---

# littleAI

Control Dave’s littleAI device via WebSocket.

Defaults (example):
- Device IP: `DEVICE_IP`
- WS endpoint: `ws://DEVICE_IP:8080/ws`

If the IP changes, ask the user for the current STA IP (it prints in serial logs after Wi‑Fi connects).

## Quick start (recommended)
Prefer using the bundled scripts (more reliable than ad-hoc snippets):

1) Create the skill venv (installs `websockets`):

```bash
bash {baseDir}/scripts/ensure_venv.sh
```

2) Speak:

```bash
source {baseDir}/.venv/bin/activate
python3 {baseDir}/scripts/speak_ws.py --ip DEVICE_IP --text "Hello Dave"
```

3) Attention (caption + blink + optional beep + optional speak):

```bash
source {baseDir}/.venv/bin/activate
python3 {baseDir}/scripts/attention.py --ip DEVICE_IP --mode normal --text "Wow!" --beep --speak
```

4) Gateway-boot greeting (yawn + randomized hello):

```bash
source {baseDir}/.venv/bin/activate
python3 {baseDir}/scripts/boot_greet.py --ip DEVICE_IP
```

## WS protocol
- Read `{baseDir}/references/ws-api.md` for the canonical message shapes.
- Remember: messages use key `type` (not `cmd`).

## Interaction conventions (Dave preferences)
- Default vibe: **friend mode** (playful is OK).
- If Dave says **"Sam, quiet mode"**: stop speaking/beeps; allow silent face/captions.
- If Dave says **"Sam, normal mode"** or **"talk again"**: resume speaking.
