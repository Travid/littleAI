#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EXPR_NEUTRAL = 0,
    EXPR_HAPPY,
    EXPR_SAD,
    EXPR_ANGRY,
    EXPR_SURPRISED,
    EXPR_THINKING,
    EXPR_SLEEPING,
} expression_t;

typedef struct {
    expression_t expression;
    float intensity;     // 0..1
    float gaze_x;        // -1..1
    float gaze_y;        // -1..1

    // Parametric "rig" controls (sticky when override=true)
    // eye_open: 0.0 (closed/squint) .. 1.0 (wide open)
    // mouth_open: 0.0 (closed line) .. 1.0 (fully open)
    float eye_open;
    float mouth_open;
    bool eye_open_override;
    bool mouth_open_override;

    char caption[96];
    uint32_t caption_until_ms;

    char viseme[8];
    float viseme_weight;
    uint32_t viseme_until_ms;

    uint32_t blink_until_ms;
} face_state_t;

void face_state_init(face_state_t *s);

#ifdef __cplusplus
}
#endif
