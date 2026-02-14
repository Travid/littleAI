#include "wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_mac.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi_mgr";

// NVS keys
static const char *kNvsNs = "wifi";
static const char *kKeySsid = "ssid";
static const char *kKeyPass = "pass";

static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;

static bool s_connected = false;
static char s_ip_str[16] = {0};

// Captive portal server state
static httpd_handle_t s_httpd = NULL;
static TaskHandle_t s_dns_task = NULL;
static TaskHandle_t s_watch_task = NULL;

static void start_softap_portal(void);
static void stop_softap_portal(void);
static void connect_sta_from_saved(void);
static void wifi_watchdog_task(void *arg);
static void dns_task(void *param);

static esp_err_t nvs_get_str_dup(const char *key, char **out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = 0;
    err = nvs_get_str(h, key, NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(h);
        return err;
    }
    char *buf = (char*)calloc(1, len);
    if (!buf) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    *out = buf;
    return ESP_OK;
}

static esp_err_t nvs_set_wifi(const char *ssid, const char *pass) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNvsNs, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, kKeySsid, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, kKeyPass, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void update_ip_cache(void) {
    if (!s_netif_sta) return;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_netif_sta, &ip) == ESP_OK) {
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ip.ip));
    }
}

bool wifi_manager_is_connected(void) {
    return s_connected;
}

const char* wifi_manager_get_ip_str(void) {
    return s_connected ? s_ip_str : NULL;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "STA disconnected");
            s_connected = false;
            // Reopen portal if we lose Wi-Fi after boot
            start_softap_portal();
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        update_ip_cache();

        stop_softap_portal();
    }
}

// -----------------------
// DNS hijack task
// -----------------------
// Responds to all A queries with 192.168.4.1.

static void dns_task(void *param) {
    (void)param;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 12) continue;

        uint16_t qdcount = ((uint16_t)buf[4] << 8) | buf[5];
        if (qdcount < 1) continue;

        // Parse only the first question.
        // QNAME starts at offset 12. Handle normal labels and (rare) compressed pointers.
        int pos = 12;
        while (pos < n) {
            uint8_t len = buf[pos];
            if (len == 0) {
                pos += 1;
                break;
            }
            if ((len & 0xC0) == 0xC0) {
                // pointer; QNAME ends here (2 bytes)
                if (pos + 1 >= n) {
                    pos = -1;
                    break;
                }
                pos += 2;
                break;
            }
            if (len > 63) {
                pos = -1;
                break;
            }
            pos += 1 + len;
        }
        if (pos < 0 || pos + 4 > n) continue;

        int qtype_pos = pos;
        uint16_t qtype = ((uint16_t)buf[qtype_pos] << 8) | buf[qtype_pos + 1];
        // uint16_t qclass = ((uint16_t)buf[qtype_pos + 2] << 8) | buf[qtype_pos + 3];

        // Response header
        // Copy RD from request, but set QR=1 and AA=1.
        uint8_t rd = (buf[2] & 0x01);
        buf[2] = (uint8_t)(0x80 | 0x04 | (rd ? 0x01 : 0x00));
        buf[3] = 0x00; // NOERROR, RA=0

        // We only include 1 question in the response payload.
        buf[4] = 0x00;
        buf[5] = 0x01;

        // NSCOUNT=0, ARCOUNT=0
        buf[8] = buf[9] = buf[10] = buf[11] = 0;

        // Default: no answers.
        buf[6] = 0x00;
        buf[7] = 0x00;

        // For captive portals, it's usually enough to only answer A queries.
        // For AAAA/others, return NOERROR with 0 answers (NODATA) to avoid negative caching.
        if (qtype != 1) {
            sendto(sock, buf, qtype_pos + 4, 0, (struct sockaddr *)&from, fromlen);
            continue;
        }

        // ANCOUNT=1
        buf[6] = 0x00;
        buf[7] = 0x01;

        // Answer starts after question (QNAME + QTYPE + QCLASS)
        int ans = qtype_pos + 4;
        if (ans + 16 > (int)sizeof(buf)) continue;

        // NAME ptr to QNAME at 0x0c
        buf[ans++] = 0xC0;
        buf[ans++] = 0x0C;
        // TYPE A
        buf[ans++] = 0x00;
        buf[ans++] = 0x01;
        // CLASS IN
        buf[ans++] = 0x00;
        buf[ans++] = 0x01;
        // TTL 30
        buf[ans++] = 0x00;
        buf[ans++] = 0x00;
        buf[ans++] = 0x00;
        buf[ans++] = 0x1E;
        // RDLENGTH 4
        buf[ans++] = 0x00;
        buf[ans++] = 0x04;
        // 192.168.4.1
        buf[ans++] = 192;
        buf[ans++] = 168;
        buf[ans++] = 4;
        buf[ans++] = 1;

        sendto(sock, buf, ans, 0, (struct sockaddr *)&from, fromlen);
    }
}

// -----------------------
// HTTP config portal
// -----------------------

static const char *kHtmlForm =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>littleAI Wi-Fi Setup</title>"
    "<style>body{font-family:system-ui;margin:24px}input{font-size:16px;padding:10px;width:100%;max-width:420px;margin:6px 0}button{font-size:16px;padding:10px 14px}</style>"
    "</head><body>"
    "<h2>Connect littleAI to Wi-Fi</h2>"
    "<form method='POST' action='/save'>"
    "<label>SSID</label><br><input name='ssid' placeholder='Wi-Fi name' required><br>"
    "<label>Password</label><br><input name='pass' type='password' placeholder='Wi-Fi password'><br>"
    "<button type='submit'>Save & Connect</button>"
    "</form>"
    "</body></html>";

static esp_err_t handle_root(httpd_req_t *req) {
    ESP_LOGI(TAG, "HTTP GET %s", req->uri);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_send(req, kHtmlForm, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t redirect_to_root(httpd_req_t *req) {
    ESP_LOGI(TAG, "HTTP redirect %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "Redirecting...");
    return ESP_OK;
}

static esp_err_t handle_any_get(httpd_req_t *req) {
    // Catch-all for captive portal probes like /generate_204, /hotspot-detect.html, etc.
    return redirect_to_root(req);
}

static void url_decode_inplace(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = {r[1], r[2], 0};
            *w++ = (char)strtol(hex, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

static esp_err_t handle_save(httpd_req_t *req) {
    ESP_LOGI(TAG, "HTTP POST %s", req->uri);
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_OK;
    }
    buf[received] = 0;

    char ssid[64] = {0};
    char pass[64] = {0};

    char *p = buf;
    while (p && *p) {
        char *amp = strchr(p, '&');
        if (amp) *amp = 0;

        char *eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            const char *k = p;
            char *v = eq + 1;
            url_decode_inplace(v);

            if (strcmp(k, "ssid") == 0) {
                strncpy(ssid, v, sizeof(ssid) - 1);
            } else if (strcmp(k, "pass") == 0) {
                strncpy(pass, v, sizeof(pass) - 1);
            }
        }

        p = amp ? (amp + 1) : NULL;
    }

    if (ssid[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Saving Wi-Fi SSID='%s'", ssid);
    esp_err_t err = nvs_set_wifi(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body><h3>Saved. Connecting...</h3><p>You can close this page.</p></body></html>");

    // Try connecting
    connect_sta_from_saved();

    return ESP_OK;
}

static void start_httpd(void) {
    if (s_httpd) return;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        s_httpd = NULL;
        return;
    }
    ESP_LOGI(TAG, "HTTP portal listening on :80");

    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &uri_root);

    // Captive portal probes: catch all GETs and redirect to '/'
    httpd_uri_t uri_any = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = handle_any_get,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &uri_any);

    httpd_uri_t uri_save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = handle_save,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_httpd, &uri_save);
}

static void stop_httpd(void) {
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

static void start_softap_portal(void) {
    if (s_netif_ap) {
        start_httpd();
        return;
    }

    ESP_LOGW(TAG, "Starting config portal AP...");

    s_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {0};
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "littleAI-setup-%02X%02X", mac[4], mac[5]);

    strncpy((char*)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    // Wi-Fi is already started in wifi_manager_start(); avoid crashing if called again.
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "esp_wifi_start (AP portal) returned: %s", esp_err_to_name(err));
    }

    start_httpd();

    if (!s_dns_task) {
        xTaskCreate(dns_task, "dns", 4096, NULL, 3, &s_dns_task);
    }

    ESP_LOGI(TAG, "AP SSID: %s", ap_ssid);
    ESP_LOGI(TAG, "Open http://192.168.4.1/");
}

static void stop_softap_portal(void) {
    stop_httpd();

    if (s_dns_task) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }

    // Important: actually stop the AP radio. If we only destroy the netif, the SSID can
    // continue to beacon but clients can't properly connect (no DHCP/netif), which is confusing.
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(e));
    }

    if (s_netif_ap) {
        esp_netif_destroy_default_wifi(s_netif_ap);
        s_netif_ap = NULL;
    }
}

static void connect_sta_from_saved(void) {
    char *ssid = NULL;
    char *pass = NULL;

    if (nvs_get_str_dup(kKeySsid, &ssid) != ESP_OK) {
        start_softap_portal();
        return;
    }
    nvs_get_str_dup(kKeyPass, &pass);

    wifi_config_t cfg = {0};
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (pass) strncpy((char*)cfg.sta.password, pass, sizeof(cfg.sta.password));

    ESP_LOGI(TAG, "Connecting STA to '%s'", ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    esp_wifi_connect();

    free(ssid);
    if (pass) free(pass);

    if (!s_watch_task) {
        xTaskCreate(wifi_watchdog_task, "wifi_watch", 3072, NULL, 2, &s_watch_task);
    }
}

static void wifi_watchdog_task(void *arg) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(12000));
        if (!wifi_manager_is_connected()) {
            ESP_LOGW(TAG, "Not connected; ensuring AP portal is running");
            start_softap_portal();
        }
    }
}

void wifi_manager_start(void) {
    static bool started = false;
    if (started) return;
    started = true;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Start Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Try STA with saved creds; if none, AP portal starts
    connect_sta_from_saved();
}
