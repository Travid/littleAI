#include "face_protocol.h"

#include <string.h>

void face_state_init(face_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->expression = EXPR_NEUTRAL;
    s->intensity = 1.0f;
    s->gaze_x = 0.0f;
    s->gaze_y = 0.0f;

    // Rig defaults (no override by default; expression/viseme drive these)
    s->eye_open = 0.8f;
    s->mouth_open = 0.0f;
    s->eye_open_override = false;
    s->mouth_open_override = false;

    strcpy(s->viseme, "rest");
    s->viseme_weight = 0.0f;
}
