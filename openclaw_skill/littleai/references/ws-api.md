# littleAI WS API (summary)

Endpoint:
- `ws://<device-ip>:8080/ws`

All messages are JSON with a top-level `type`.

## Core
- `ping`
- `get_state`

## Face
- `set_expression`: `{type, expression, intensity?}`
- `gaze`: `{type, x, y}` (range -1..1)
- `blink`: `{type, duration_ms}`
- `caption`: `{type, text, ttl_ms}`
- `viseme`: `{type, name, weight, ttl_ms}`

## Rig (sticky overrides)
- `eyes`: `{type:"eyes", open:0..1, override?:bool}`
- `mouth`: `{type:"mouth", open:0..1, override?:bool}`
- `rig`: `{type:"rig", eye_open?:0..1, mouth_open?:0..1}`
- `rig_clear`: `{type:"rig_clear"}`

## Audio
- `beep`: `{type:"beep", freq_hz, duration_ms}`
- `speak_pcm`: `{type:"speak_pcm", data_b64:"..."}` (PCM16LE mono @ 16kHz, chunked)
