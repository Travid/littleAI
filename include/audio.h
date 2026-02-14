#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sample_rate_hz;   // e.g. 16000
    int volume_percent;   // 0..100
} audio_config_t;

// Initialize ES8311 + I2S speaker output.
esp_err_t audio_init(const audio_config_t *cfg);

// Play 16-bit signed little-endian mono PCM at the configured sample rate.
esp_err_t audio_play_pcm16_mono(const int16_t *samples, size_t sample_count);

// Convenience test sound.
esp_err_t audio_beep(int freq_hz, int duration_ms);

#ifdef __cplusplus
}
#endif
