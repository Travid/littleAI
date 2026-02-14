#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "face_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    face_state_t *face;
    SemaphoreHandle_t face_mutex;
} ws_server_config_t;

// Start a WebSocket server on http://<ip>:8080/ws
// Incoming JSON commands update the provided face state.
esp_err_t ws_server_start(const ws_server_config_t *cfg);

#ifdef __cplusplus
}
#endif
