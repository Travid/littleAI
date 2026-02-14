#include "audio.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_check.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"

#include "es8311.h"

#include "pin_config.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx = NULL;
static es8311_handle_t s_es = NULL;
static int s_sample_rate = 16000;

static esp_err_t pa_enable(bool on)
{
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PA_ENABLE),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "gpio_config(PA_ENABLE) failed");
    gpio_set_level(PA_ENABLE, on ? 1 : 0);
    return ESP_OK;
}

esp_err_t audio_init(const audio_config_t *cfg)
{
    int volume = 75;
    if (cfg) {
        if (cfg->sample_rate_hz > 0) s_sample_rate = cfg->sample_rate_hz;
        if (cfg->volume_percent >= 0) volume = cfg->volume_percent;
    }
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    ESP_RETURN_ON_ERROR(pa_enable(true), TAG, "PA enable failed");

    // ---- I2S TX ----
    if (!s_tx) {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;
        ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s_new_channel failed");

        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_sample_rate),
            // ES8311 example uses stereo slots; we'll duplicate mono samples into L+R.
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_MCK_IO,
                .bclk = I2S_BCK_IO,
                .ws   = I2S_WS_IO,
                .dout = I2S_DO_IO,
                .din  = I2S_DI_IO,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv   = false,
                },
            },
        };
        // Common multiple for audio codecs
        std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s_channel_init_std_mode failed");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s_channel_enable failed");
    }

    // ---- ES8311 ----
    if (!s_es) {
        // ES8311 address: CE low -> 0x18. Your I2C scan shows 0x18 present.
        // Uses I2C_NUM_0 (same bus as touch).
        s_es = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
        ESP_RETURN_ON_FALSE(s_es, ESP_FAIL, TAG, "es8311_create failed");

        const es8311_clock_config_t es_clk = {
            .mclk_inverted = false,
            .sclk_inverted = false,
            .mclk_from_mclk_pin = true,
            .mclk_frequency = s_sample_rate * 256,
            .sample_frequency = s_sample_rate,
        };

        ESP_RETURN_ON_ERROR(es8311_init(s_es, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "es8311_init failed");
        ESP_RETURN_ON_ERROR(es8311_sample_frequency_config(s_es, s_sample_rate * 256, s_sample_rate), TAG, "es8311_sample_frequency_config failed");
        ESP_RETURN_ON_ERROR(es8311_voice_volume_set(s_es, volume, NULL), TAG, "es8311_voice_volume_set failed");
        ESP_RETURN_ON_ERROR(es8311_microphone_config(s_es, false), TAG, "es8311_microphone_config failed");

        ESP_LOGI(TAG, "Audio init OK (sr=%d, vol=%d%%)", s_sample_rate, volume);
    }

    return ESP_OK;
}

esp_err_t audio_play_pcm16_mono(const int16_t *samples, size_t sample_count)
{
    if (!s_tx || !samples || sample_count == 0) return ESP_ERR_INVALID_STATE;

    // Duplicate mono into stereo in small chunks to keep RAM low.
    int16_t stereo[256 * 2];
    size_t idx = 0;
    while (idx < sample_count) {
        size_t n = sample_count - idx;
        if (n > 256) n = 256;
        for (size_t i = 0; i < n; i++) {
            int16_t s = samples[idx + i];
            stereo[i * 2 + 0] = s;
            stereo[i * 2 + 1] = s;
        }

        size_t bytes_written = 0;
        esp_err_t e = i2s_channel_write(s_tx, stereo, n * 2 * sizeof(int16_t), &bytes_written, pdMS_TO_TICKS(2000));
        if (e != ESP_OK) return e;
        idx += n;
    }

    return ESP_OK;
}

esp_err_t audio_beep(int freq_hz, int duration_ms)
{
    if (freq_hz <= 0) freq_hz = 880;
    if (duration_ms <= 0) duration_ms = 120;
    if (duration_ms > 2000) duration_ms = 2000;

    const float amp = 0.25f; // keep it gentle
    const int total = (s_sample_rate * duration_ms) / 1000;

    int16_t buf[256];
    int produced = 0;
    while (produced < total) {
        int n = total - produced;
        if (n > (int)(sizeof(buf)/sizeof(buf[0]))) n = (int)(sizeof(buf)/sizeof(buf[0]));

        for (int i = 0; i < n; i++) {
            float t = (float)(produced + i) / (float)s_sample_rate;
            float v = sinf(2.0f * (float)M_PI * (float)freq_hz * t);
            int32_t s = (int32_t)(v * amp * 32767.0f);
            buf[i] = (int16_t)s;
        }

        ESP_RETURN_ON_ERROR(audio_play_pcm16_mono(buf, (size_t)n), TAG, "beep write failed");
        produced += n;
    }

    return ESP_OK;
}
