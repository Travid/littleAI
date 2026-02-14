#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts Wi-Fi in STA mode if credentials are saved.
// If connection fails (or no creds), it starts a SoftAP + config portal.
//
// This call is non-blocking: it spawns tasks/event handlers.
void wifi_manager_start(void);

// Returns true if STA is connected and has an IP.
bool wifi_manager_is_connected(void);

// Get current STA IP as string (pointer valid until next call). Returns NULL if not connected.
const char* wifi_manager_get_ip_str(void);

#ifdef __cplusplus
}
#endif
