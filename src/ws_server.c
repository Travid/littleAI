#include "ws_server.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"

#include "mbedtls/base64.h"

#include "cJSON.h"

#include "audio.h"

static const char *TAG = "ws";

static httpd_handle_t s_httpd = NULL;
static face_state_t *s_face = NULL;
static SemaphoreHandle_t s_face_mux = NULL;

static const char* expr_to_str(expression_t e) {
    switch (e) {
        case EXPR_NEUTRAL: return "neutral";
        case EXPR_HAPPY: return "happy";
        case EXPR_SAD: return "sad";
        case EXPR_ANGRY: return "angry";
        case EXPR_SURPRISED: return "surprised";
        case EXPR_THINKING: return "thinking";
        case EXPR_SLEEPING: return "sleeping";
        default: return "neutral";
    }
}

static bool str_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static expression_t parse_expr(const char *s) {
    if (!s) return EXPR_NEUTRAL;
    if (str_eq(s, "neutral")) return EXPR_NEUTRAL;
    if (str_eq(s, "happy")) return EXPR_HAPPY;
    if (str_eq(s, "sad")) return EXPR_SAD;
    if (str_eq(s, "angry")) return EXPR_ANGRY;
    if (str_eq(s, "surprised")) return EXPR_SURPRISED;
    if (str_eq(s, "thinking")) return EXPR_THINKING;
    if (str_eq(s, "sleeping")) return EXPR_SLEEPING;
    return EXPR_NEUTRAL;
}

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static cJSON* face_to_json(const face_state_t *f) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "expression", expr_to_str(f->expression));
    cJSON_AddNumberToObject(o, "intensity", f->intensity);
    cJSON_AddNumberToObject(o, "gaze_x", f->gaze_x);
    cJSON_AddNumberToObject(o, "gaze_y", f->gaze_y);
    cJSON_AddNumberToObject(o, "eye_open", f->eye_open);
    cJSON_AddBoolToObject(o, "eye_open_override", f->eye_open_override);
    cJSON_AddNumberToObject(o, "mouth_open", f->mouth_open);
    cJSON_AddBoolToObject(o, "mouth_open_override", f->mouth_open_override);

    cJSON_AddStringToObject(o, "caption", f->caption);
    cJSON_AddNumberToObject(o, "caption_until_ms", (double)f->caption_until_ms);
    cJSON_AddStringToObject(o, "viseme", f->viseme);
    cJSON_AddNumberToObject(o, "viseme_weight", f->viseme_weight);
    cJSON_AddNumberToObject(o, "viseme_until_ms", (double)f->viseme_until_ms);
    cJSON_AddNumberToObject(o, "blink_until_ms", (double)f->blink_until_ms);
    return o;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *obj) {
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) return ESP_ERR_NO_MEM;

    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)s,
        .len = strlen(s),
    };

    esp_err_t err = httpd_ws_send_frame(req, &frame);
    free(s);
    return err;
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WS handshake OK");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws recv (len) failed: %s", esp_err_to_name(err));
        return err;
    }

    if (frame.len == 0) {
        return ESP_OK;
    }

    if (frame.len > 16384) {
        ESP_LOGW(TAG, "ws payload too large: %u", (unsigned)frame.len);
        return ESP_OK;
    }

    char *buf = (char *)calloc(1, frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    frame.payload = (uint8_t *)buf;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ws recv failed: %s", esp_err_to_name(err));
        free(buf);
        return err;
    }
    buf[frame.len] = 0;

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    cJSON *resp = cJSON_CreateObject();

    if (!root || !cJSON_IsObject(root)) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "invalid_json");
        cJSON_Delete(root);
        esp_err_t se = send_json(req, resp);
        cJSON_Delete(resp);
        return se;
    }

    const cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : NULL;

    if (!t) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "missing_type");
        cJSON_Delete(root);
        esp_err_t se = send_json(req, resp);
        cJSON_Delete(resp);
        return se;
    }

    // ---------- Commands ----------
    uint32_t n = now_ms();

    if (str_eq(t, "ping")) {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "type", "pong");
        cJSON_AddNumberToObject(resp, "ts_ms", (double)n);
    } else if (str_eq(t, "get_state")) {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "type", "state");
        if (s_face && s_face_mux && xSemaphoreTake(s_face_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
            cJSON_AddItemToObject(resp, "state", face_to_json(s_face));
            xSemaphoreGive(s_face_mux);
        } else {
            cJSON_AddStringToObject(resp, "error", "face_unavailable");
        }
    } else if (str_eq(t, "beep")) {
        const cJSON *freq = cJSON_GetObjectItem(root, "freq_hz");
        const cJSON *dur = cJSON_GetObjectItem(root, "duration_ms");
        int f = cJSON_IsNumber(freq) ? (int)freq->valuedouble : 880;
        int d = cJSON_IsNumber(dur) ? (int)dur->valuedouble : 140;
        esp_err_t ae = audio_beep(f, d);
        cJSON_AddBoolToObject(resp, "ok", ae == ESP_OK);
        cJSON_AddStringToObject(resp, "type", "ack");
        cJSON_AddStringToObject(resp, "cmd", "beep");
        if (ae != ESP_OK) cJSON_AddStringToObject(resp, "error", esp_err_to_name(ae));
    } else if (str_eq(t, "speak_pcm")) {
        // PCM16 LE mono chunk (base64). Use multiple messages to stream longer speech.
        const cJSON *b64 = cJSON_GetObjectItem(root, "data_b64");
        if (!cJSON_IsString(b64) || !b64->valuestring) {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "type", "ack");
            cJSON_AddStringToObject(resp, "cmd", "speak_pcm");
            cJSON_AddStringToObject(resp, "error", "missing_data_b64");
        } else {
            size_t in_len = strlen(b64->valuestring);
            size_t out_len = 0;
            // upper bound: 3/4 of input + a little
            size_t out_cap = (in_len / 4) * 3 + 8;
            uint8_t *out = (uint8_t *)calloc(1, out_cap);
            if (!out) {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "type", "ack");
                cJSON_AddStringToObject(resp, "cmd", "speak_pcm");
                cJSON_AddStringToObject(resp, "error", "no_mem");
            } else {
                int mbed = mbedtls_base64_decode(out, out_cap, &out_len, (const unsigned char *)b64->valuestring, in_len);
                if (mbed != 0 || out_len < 2) {
                    cJSON_AddBoolToObject(resp, "ok", false);
                    cJSON_AddStringToObject(resp, "type", "ack");
                    cJSON_AddStringToObject(resp, "cmd", "speak_pcm");
                    cJSON_AddStringToObject(resp, "error", "bad_base64");
                } else {
                    // Ensure 16-bit alignment
                    out_len &= ~((size_t)1);
                    esp_err_t ae = audio_play_pcm16_mono((const int16_t *)out, out_len / 2);
                    cJSON_AddBoolToObject(resp, "ok", ae == ESP_OK);
                    cJSON_AddStringToObject(resp, "type", "ack");
                    cJSON_AddStringToObject(resp, "cmd", "speak_pcm");
                    if (ae != ESP_OK) cJSON_AddStringToObject(resp, "error", esp_err_to_name(ae));
                }
                free(out);
            }
        }
    } else {
        bool updated = false;
        if (!s_face || !s_face_mux) {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", "face_unavailable");
        } else if (xSemaphoreTake(s_face_mux, pdMS_TO_TICKS(50)) != pdTRUE) {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", "face_busy");
        } else {
            if (str_eq(t, "set_expression")) {
                const cJSON *expr = cJSON_GetObjectItem(root, "expression");
                const cJSON *intensity = cJSON_GetObjectItem(root, "intensity");
                if (cJSON_IsString(expr)) {
                    s_face->expression = parse_expr(expr->valuestring);
                    updated = true;
                }
                if (cJSON_IsNumber(intensity)) {
                    s_face->intensity = clampf((float)intensity->valuedouble, 0.0f, 1.0f);
                    updated = true;
                }
            } else if (str_eq(t, "gaze")) {
                const cJSON *x = cJSON_GetObjectItem(root, "x");
                const cJSON *y = cJSON_GetObjectItem(root, "y");
                if (cJSON_IsNumber(x)) { s_face->gaze_x = clampf((float)x->valuedouble, -1.0f, 1.0f); updated = true; }
                if (cJSON_IsNumber(y)) { s_face->gaze_y = clampf((float)y->valuedouble, -1.0f, 1.0f); updated = true; }
            } else if (str_eq(t, "caption")) {
                const cJSON *text = cJSON_GetObjectItem(root, "text");
                const cJSON *ttl = cJSON_GetObjectItem(root, "ttl_ms");
                if (cJSON_IsString(text)) {
                    strncpy(s_face->caption, text->valuestring, sizeof(s_face->caption) - 1);
                    s_face->caption[sizeof(s_face->caption) - 1] = 0;
                    updated = true;
                }
                if (cJSON_IsNumber(ttl)) {
                    uint32_t ttl_ms = (uint32_t)ttl->valuedouble;
                    s_face->caption_until_ms = ttl_ms ? (n + ttl_ms) : 0;
                    updated = true;
                }
            } else if (str_eq(t, "viseme")) {
                const cJSON *name = cJSON_GetObjectItem(root, "name");
                const cJSON *weight = cJSON_GetObjectItem(root, "weight");
                const cJSON *ttl = cJSON_GetObjectItem(root, "ttl_ms");
                if (cJSON_IsString(name)) {
                    strncpy(s_face->viseme, name->valuestring, sizeof(s_face->viseme) - 1);
                    s_face->viseme[sizeof(s_face->viseme) - 1] = 0;
                    updated = true;
                }
                if (cJSON_IsNumber(weight)) {
                    s_face->viseme_weight = clampf((float)weight->valuedouble, 0.0f, 1.0f);
                    updated = true;
                }
                if (cJSON_IsNumber(ttl)) {
                    uint32_t ttl_ms = (uint32_t)ttl->valuedouble;
                    s_face->viseme_until_ms = ttl_ms ? (n + ttl_ms) : 0;
                    updated = true;
                }
            } else if (str_eq(t, "blink")) {
                const cJSON *dur = cJSON_GetObjectItem(root, "duration_ms");
                uint32_t d = cJSON_IsNumber(dur) ? (uint32_t)dur->valuedouble : 150;
                if (d > 2000) d = 2000;
                s_face->blink_until_ms = n + d;
                updated = true;
            } else if (str_eq(t, "eyes")) {
                const cJSON *open = cJSON_GetObjectItem(root, "open");
                const cJSON *ovr = cJSON_GetObjectItem(root, "override");
                if (cJSON_IsNumber(open)) {
                    s_face->eye_open = clampf((float)open->valuedouble, 0.0f, 1.0f);
                    s_face->eye_open_override = true;
                    updated = true;
                }
                if (cJSON_IsBool(ovr)) {
                    s_face->eye_open_override = cJSON_IsTrue(ovr);
                    updated = true;
                }
            } else if (str_eq(t, "mouth")) {
                const cJSON *open = cJSON_GetObjectItem(root, "open");
                const cJSON *ovr = cJSON_GetObjectItem(root, "override");
                if (cJSON_IsNumber(open)) {
                    s_face->mouth_open = clampf((float)open->valuedouble, 0.0f, 1.0f);
                    s_face->mouth_open_override = true;
                    updated = true;
                }
                if (cJSON_IsBool(ovr)) {
                    s_face->mouth_open_override = cJSON_IsTrue(ovr);
                    updated = true;
                }
            } else if (str_eq(t, "rig")) {
                // Set both eye_open and mouth_open in one message
                const cJSON *eo = cJSON_GetObjectItem(root, "eye_open");
                const cJSON *mo = cJSON_GetObjectItem(root, "mouth_open");
                if (cJSON_IsNumber(eo)) {
                    s_face->eye_open = clampf((float)eo->valuedouble, 0.0f, 1.0f);
                    s_face->eye_open_override = true;
                    updated = true;
                }
                if (cJSON_IsNumber(mo)) {
                    s_face->mouth_open = clampf((float)mo->valuedouble, 0.0f, 1.0f);
                    s_face->mouth_open_override = true;
                    updated = true;
                }
            } else if (str_eq(t, "rig_clear")) {
                s_face->eye_open_override = false;
                s_face->mouth_open_override = false;
                updated = true;
            } else if (str_eq(t, "set_state")) {
                // Convenience: set multiple fields at once.
                const cJSON *st = cJSON_GetObjectItem(root, "state");
                if (cJSON_IsObject(st)) {
                    const cJSON *expr = cJSON_GetObjectItem(st, "expression");
                    const cJSON *intensity = cJSON_GetObjectItem(st, "intensity");
                    const cJSON *gx = cJSON_GetObjectItem(st, "gaze_x");
                    const cJSON *gy = cJSON_GetObjectItem(st, "gaze_y");
                    const cJSON *cap = cJSON_GetObjectItem(st, "caption");
                    const cJSON *cap_ttl = cJSON_GetObjectItem(st, "caption_ttl_ms");
                    const cJSON *eo = cJSON_GetObjectItem(st, "eye_open");
                    const cJSON *eo_ovr = cJSON_GetObjectItem(st, "eye_open_override");
                    const cJSON *mo = cJSON_GetObjectItem(st, "mouth_open");
                    const cJSON *mo_ovr = cJSON_GetObjectItem(st, "mouth_open_override");

                    if (cJSON_IsString(expr)) { s_face->expression = parse_expr(expr->valuestring); updated = true; }
                    if (cJSON_IsNumber(intensity)) { s_face->intensity = clampf((float)intensity->valuedouble, 0.0f, 1.0f); updated = true; }
                    if (cJSON_IsNumber(gx)) { s_face->gaze_x = clampf((float)gx->valuedouble, -1.0f, 1.0f); updated = true; }
                    if (cJSON_IsNumber(gy)) { s_face->gaze_y = clampf((float)gy->valuedouble, -1.0f, 1.0f); updated = true; }

                    if (cJSON_IsNumber(eo)) { s_face->eye_open = clampf((float)eo->valuedouble, 0.0f, 1.0f); updated = true; }
                    if (cJSON_IsBool(eo_ovr)) { s_face->eye_open_override = cJSON_IsTrue(eo_ovr); updated = true; }
                    if (cJSON_IsNumber(mo)) { s_face->mouth_open = clampf((float)mo->valuedouble, 0.0f, 1.0f); updated = true; }
                    if (cJSON_IsBool(mo_ovr)) { s_face->mouth_open_override = cJSON_IsTrue(mo_ovr); updated = true; }

                    if (cJSON_IsString(cap)) {
                        strncpy(s_face->caption, cap->valuestring, sizeof(s_face->caption) - 1);
                        s_face->caption[sizeof(s_face->caption) - 1] = 0;
                        updated = true;
                    }
                    if (cJSON_IsNumber(cap_ttl)) {
                        uint32_t ttl_ms = (uint32_t)cap_ttl->valuedouble;
                        s_face->caption_until_ms = ttl_ms ? (n + ttl_ms) : 0;
                        updated = true;
                    }
                }
            } else {
                // Unknown command
            }

            cJSON_AddBoolToObject(resp, "ok", updated);
            if (!updated) {
                cJSON_AddStringToObject(resp, "error", "unknown_or_invalid_command");
            }

            cJSON_AddStringToObject(resp, "type", "ack");
            cJSON_AddStringToObject(resp, "cmd", t);
            cJSON_AddNumberToObject(resp, "ts_ms", (double)n);
            cJSON_AddItemToObject(resp, "state", face_to_json(s_face));
            xSemaphoreGive(s_face_mux);
        }
    }

    cJSON_Delete(root);

    esp_err_t send_err = send_json(req, resp);
    cJSON_Delete(resp);
    return send_err;
}

esp_err_t ws_server_start(const ws_server_config_t *cfg) {
    if (s_httpd) return ESP_OK;

    if (!cfg || !cfg->face || !cfg->face_mutex) {
        ESP_LOGE(TAG, "ws_server_start: missing cfg/face/mutex");
        return ESP_ERR_INVALID_ARG;
    }

    s_face = cfg->face;
    s_face_mux = cfg->face_mutex;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting WS server on :%d/ws", config.server_port);

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return err;
    }

    httpd_uri_t ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_httpd, &ws);

    return ESP_OK;
}
