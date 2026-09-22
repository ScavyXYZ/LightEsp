#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "ota_update.h"

#define LED_GPIO GPIO_NUM_4
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define MAX_AP_RECORDS 10
#define DNS_PORT 53

/* DNS header field offsets/values, named instead of magic numbers */
#define DNS_HEADER_SIZE          12
#define DNS_ANSWER_SIZE          16
#define DNS_FLAGS_HI_OFFSET      2
#define DNS_FLAGS_LO_OFFSET      3
#define DNS_FLAGS_HI_RESPONSE    0x81   /* QR=1, Opcode=0, AA=0, TC=0, RD=1 */
#define DNS_FLAGS_LO_RESPONSE    0x80   /* RA=1, Z=0, RCODE=0 */
#define DNS_QDCOUNT_HI_OFFSET    6
#define DNS_QDCOUNT_LO_OFFSET    7
#define DNS_ANCOUNT_HI_OFFSET    8
#define DNS_ANCOUNT_LO_OFFSET    9
#define DNS_NSCOUNT_HI_OFFSET    10
#define DNS_NSCOUNT_LO_OFFSET    11
#define DNS_MAX_PACKET_SIZE      128

static const char *TAG = "wifi_app";
static httpd_handle_t server = NULL;
static bool led_state = false;
static TaskHandle_t dns_task_handle = NULL;

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_AUTH_FAIL_BIT BIT2

#define WIFI_CONNECT_MAX_RETRIES 5
static int wifi_retry_count = 0;

/* Guards against concurrent /save requests triggering overlapping
 * Wi-Fi connection attempts and duplicate event-handler registration.
 * Also guards the Wi-Fi scan in send_config_page(), since esp_wifi_scan_start()
 * and esp_wifi_connect()/disconnect() issued concurrently on the same
 * driver instance are a known source of asserts/panics in the Wi-Fi stack. */
static SemaphoreHandle_t wifi_test_mutex = NULL;

/* True only while we're in AP+STA setup mode waiting for the user to
 * submit credentials. The permanent wifi_event_handler uses this to
 * avoid racing with the temporary test-connection handler. */
static volatile bool in_setup_mode = false;

/* Shared style block used by every page so the whole flow (control,
 * wifi setup, factory reset) feels like one product instead of three
 * disconnected admin forms. Deliberately plain: no cards, gradients,
 * or shadows — flat background, one accent color, thin hairline rules. */
static const char* shared_style_css =
"*{box-sizing:border-box;}"
":root{"
"  --bg:#111214; --ink-0:#f2f2f0; --ink-1:#9a9a96; --ink-2:#5c5c58;"
"  --accent:#e8a33d; --border:#2a2a28; --red:#e2665a;"
"}"
"body{"
"  margin:0; min-height:100vh; display:flex; flex-direction:column; align-items:center;"
"  justify-content:center; padding:40px 24px;"
"  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif;"
"  background:var(--bg); color:var(--ink-0); text-align:center;"
"}"
".wrap{width:100%%; max-width:320px;}"
".eyebrow{"
"  font-size:11px; letter-spacing:.14em; text-transform:uppercase;"
"  color:var(--ink-2); margin:0 0 8px;"
"}"
"h1,h2{margin:0 0 6px; font-weight:500; letter-spacing:-.01em;}"
"h1{font-size:20px;} h2{font-size:18px;}"
".subtitle{color:var(--ink-1); font-size:13px; margin:0 0 36px;}"
"hr{border:none; border-top:1px solid var(--border); margin:28px 0;}"
".btn{"
"  appearance:none; border:none; cursor:pointer;"
"  font-size:14px; font-weight:500; padding:14px 20px;"
"  background:none; color:var(--ink-0); border:1px solid var(--border);"
"  transition:border-color .15s ease, color .15s ease;"
"  text-decoration:none; display:block; width:100%%;"
"}"
".btn:hover{border-color:var(--ink-2);}"
".btn:disabled{opacity:.5; cursor:default;}"
".btn-primary{background:var(--accent); color:#151107; border-color:var(--accent); font-weight:600;}"
".btn-danger{color:var(--red); border-color:var(--border);}"
".btn-danger:hover{border-color:var(--red);}"
".stack{display:flex; flex-direction:column; gap:10px;}"
".footer-note{margin-top:28px; font-size:12px; color:var(--ink-2);}"
".footer-note a{color:var(--ink-1);}"
"label{display:block; text-align:left; font-size:12px; color:var(--ink-1); margin:22px 0 8px; letter-spacing:.02em;}"
"input,select{"
"  width:100%%; padding:12px 0; border:none; border-bottom:1px solid var(--border);"
"  background:none; color:var(--ink-0); font-size:14px; outline:none; border-radius:0;"
"  transition:border-color .15s ease;"
"}"
"input:focus,select:focus{border-color:var(--accent);}"
"select{appearance:none;}"
".status-box,.error-box{"
"  font-size:13px; text-align:left; margin-bottom:20px; padding-bottom:16px;"
"  border-bottom:1px solid var(--border); display:none;"
"}"
".error-box{display:block; color:var(--red);}"
".status-box{color:var(--ink-1);}"
".version-row{"
"  display:flex; justify-content:space-between; align-items:center;"
"  font-size:12px; color:var(--ink-2); margin-top:4px;"
"}"
;

/* --- Control (home) page: just the orb and two buttons underneath.
 * No headings or labels — the glow itself communicates state. */
static const char* control_page_html =
"<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
"<meta charset='utf-8'><title>Lamp</title>"
"<style>"
"%s"
".lamp-orb{"
"  width:180px; height:180px; border-radius:50%%; border:none; cursor:pointer;"
"  background:#1a1b1e; box-shadow:inset 0 0 0 1px var(--border);"
"  transition:background .25s ease, box-shadow .25s ease, transform .15s ease;"
"  margin-bottom:32px;"
"}"
".lamp-orb:active{transform:scale(.97);}"
".lamp-orb:disabled{cursor:default;}"
".lamp-orb.on{"
"  background:radial-gradient(circle at 38%% 32%%, #ffe2ad, var(--accent) 70%%);"
"  box-shadow:inset 0 0 0 1px rgba(255,255,255,.35), 0 0 60px rgba(232,163,61,.45), 0 0 120px rgba(232,163,61,.25);"
"}"
"</style></head><body>"
"<div class='wrap'>"
"<button id='toggleBtn' class='lamp-orb %s' onclick='toggleLed()' aria-label='Toggle lamp'></button>"
"<div class='stack'>"
"<a href='/wifi' class='btn'>Wi&#8209;Fi settings</a>"
"<a href='/reset' class='btn btn-danger'>Factory reset</a>"
"</div>"
"</div>"
"<script>"
"async function toggleLed() {"
"  const btn = document.getElementById('toggleBtn');"
"  btn.disabled = true;"
"  try {"
"    const resp = await fetch('/toggle', { method: 'POST' });"
"    if (!resp.ok) throw new Error('request failed');"
"    const data = await resp.json();"
"    btn.classList.toggle('on', data.state);"
"  } catch (e) {"
"    console.error(e);"
"  } finally {"
"    btn.disabled = false;"
"  }"
"}"
"</script>"
"</body></html>";

void start_ap_mode(void);
void start_web_server(void);
void start_config_server(void);
static void start_mdns_service(void);
static void start_dns_server(void);
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data);
static void blink_error_led(void);

static void html_escape(const char *src, char *dst, size_t dst_size) {
    size_t di = 0;
    if (dst_size == 0) return;
    for (size_t si = 0; src[si] != '\0'; si++) {
        unsigned char c = (unsigned char)src[si];
        const char *rep = NULL;
        switch (c) {
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '&':  rep = "&amp;";  break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default:   break;
        }
        if (rep) {
            size_t rep_len = strlen(rep);
            if (di + rep_len >= dst_size) break; /* would overflow, stop here */
            memcpy(dst + di, rep, rep_len);
            di += rep_len;
        } else {
            if (di + 1 >= dst_size) break;
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
}

static void url_decode(const char *src, char *dst, size_t dst_size) {
    size_t di = 0;
    if (dst_size == 0) return;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_size; si++) {
        if (src[si] == '%' &&
            isxdigit((unsigned char)src[si + 1]) &&
            isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static esp_err_t toggle_handler(httpd_req_t *req) {
    led_state = !led_state;
    gpio_set_level(LED_GPIO, led_state ? 0 : 1);

    char resp[32];
    int n = snprintf(resp, sizeof(resp), "{\"state\":%s}", led_state ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, n);
    return ESP_OK;
}

static esp_err_t root_handler(httpd_req_t *req) {
    /* Sized to comfortably fit the shared CSS block plus the page markup;
     * computed at runtime so it can't silently drift out of sync with
     * shared_style_css and re-trigger -Werror=format-truncation. */
    size_t resp_capacity = strlen(control_page_html) + strlen(shared_style_css) + 256;
    char *resp = malloc(resp_capacity);
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    const char *orb_class = led_state ? "on" : "";

    int n = snprintf(resp, resp_capacity, control_page_html,
                      shared_style_css, orb_class);
    if (n < 0 || (size_t)n >= resp_capacity) {
        free(resp);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return ESP_OK;
}

static esp_err_t send_config_page(httpd_req_t *req, const char *error_message) {
    wifi_scan_config_t scan_config = { .ssid = 0, .bssid = 0, .channel = 0, .show_hidden = false };

    /* Serialize scanning against try_wifi_connection(): esp_wifi_scan_start()
     * issued while a connect/disconnect sequence is in flight on the same
     * driver instance is a known source of Wi-Fi stack asserts/panics.
     * If we can't get the lock quickly, just skip the scan for this page
     * load rather than risking a racy call into the driver. */
    esp_err_t scan_err = ESP_FAIL;
    uint16_t ap_count = 0;
    wifi_ap_record_t ap_info[MAX_AP_RECORDS];
    memset(ap_info, 0, sizeof(ap_info));

    if (xSemaphoreTake(wifi_test_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        scan_err = esp_wifi_scan_start(&scan_config, true);
        if (scan_err == ESP_OK) {
            ap_count = MAX_AP_RECORDS;
            esp_wifi_scan_get_ap_records(&ap_count, ap_info);
            if (ap_count > MAX_AP_RECORDS) ap_count = MAX_AP_RECORDS;
        } else {
            ESP_LOGW(TAG, "Network scan failed: %s", esp_err_to_name(scan_err));
            ap_count = 0;
        }
        xSemaphoreGive(wifi_test_mutex);
    } else {
        ESP_LOGW(TAG, "Skipping scan: connection attempt in progress");
        ap_count = 0;
    }

    size_t resp_capacity = 1536 + (size_t)ap_count * 256 + strlen(shared_style_css);
    char *resp = malloc(resp_capacity);
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char error_html[560] = "";
    if (error_message) {
        char esc[400];
        html_escape(error_message, esc, sizeof(esc));
        snprintf(error_html, sizeof(error_html),
            "<div class='error-box' style='display:block;'>&#9888; %s</div>", esc);
    }

    int offset = snprintf(resp, resp_capacity,
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
        "<meta charset='utf-8'><title>Wi-Fi Setup</title>"
        "<style>%s"
        ".signal{float:right; color:var(--ink-2); font-size:12px;}"
        "</style></head><body>"
        "<div class='wrap'>"
        "<p class='eyebrow'>Smart Lamp</p>"
        "<h1>Connect to Wi&#8209;Fi</h1>"
        "<p class='subtitle'>Choose your network and enter the password</p>"
        "%s"
        "<div id='statusBox' class='status-box'></div>"
        "<form id='wifiForm' action='/save' method='POST'>"
        "<label>Network</label><select name='ssid'>",
        shared_style_css, error_html);

    if (offset < 0) {
        free(resp);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char ssid_raw[33];
    char ssid_esc[132];

    for (int i = 0; i < ap_count && (size_t)offset < resp_capacity; i++) {
        memset(ssid_raw, 0, sizeof(ssid_raw));
        memcpy(ssid_raw, ap_info[i].ssid, sizeof(ap_info[i].ssid));
        ssid_raw[sizeof(ssid_raw) - 1] = '\0';

        html_escape(ssid_raw, ssid_esc, sizeof(ssid_esc));

        int written = snprintf(resp + offset, resp_capacity - offset,
            "<option value='%s'>%s &nbsp;&middot;&nbsp; %d dBm</option>", ssid_esc, ssid_esc, ap_info[i].rssi);
        if (written < 0) break;
        /* snprintf can report more than the space left; clamp so offset
         * never exceeds resp_capacity (avoids an unsigned underflow on
         * the next iteration's resp_capacity - offset). */
        offset += written;
        if ((size_t)offset > resp_capacity) offset = (int)resp_capacity;
    }

    if (ap_count == 0 && (size_t)offset < resp_capacity) {
        /* Always give the user a usable option even if the scan was
         * skipped or failed, so they aren't stuck with an empty <select>. */
        offset += snprintf(resp + offset, resp_capacity - offset,
            "<option value=''>(no networks found — refresh to rescan)</option>");
    }

    if ((size_t)offset < resp_capacity) {
        offset += snprintf(resp + offset, resp_capacity - offset,
            "</select>"
            "<label>Password</label><input type='password' name='password' id='password' placeholder='Leave blank for open network'>"
            "<button type='submit' class='btn btn-primary' style='margin-top:28px;' id='submitBtn'>Save &amp; connect</button>"
            "</form>"
            "<hr>"
            "<div>"
            "<div class='version-row'><span>Firmware</span><span id='fwVersion'>%s</span></div>"
            "<div id='updateStatus' class='version-row' style='display:none;'></div>"
            "<button id='updateBtn' class='btn' style='margin-top:14px;' onclick='checkUpdate()'>Check for updates</button>"
            "</div>"
            "<hr>"
            "<p class='footer-note'><a href='/'>&larr; Back to lamp</a></p>"
            "</div>"
            "<script>"
            "const form = document.getElementById('wifiForm');"
            "const statusBox = document.getElementById('statusBox');"
            "const submitBtn = document.getElementById('submitBtn');"
            "form.addEventListener('submit', async (e) => {"
            "  e.preventDefault();"
            "  submitBtn.disabled = true;"
            "  statusBox.style.display = 'block';"
            "  statusBox.style.color = 'var(--ink-1)';"
            "  statusBox.textContent = 'Connecting...';"
            "  try {"
            "    const resp = await fetch('/save', { method: 'POST', body: new URLSearchParams(new FormData(form)) });"
            "    const data = await resp.json();"
            "    if (data.ok) {"
            "      statusBox.style.color = 'var(--accent)';"
            "      statusBox.textContent = data.message || 'Connected! Rebooting...';"
            "    } else {"
            "      statusBox.style.color = 'var(--red)';"
            "      statusBox.textContent = data.message || 'Connection failed.';"
            "      submitBtn.disabled = false;"
            "    }"
            "  } catch (err) {"
            "    statusBox.style.color = 'var(--red)';"
            "    statusBox.textContent = 'Request failed. Try again.';"
            "    submitBtn.disabled = false;"
            "  }"
            "});"
            "async function checkUpdate() {"
            "  const btn = document.getElementById('updateBtn');"
            "  const status = document.getElementById('updateStatus');"
            "  btn.disabled = true;"
            "  status.style.display = 'flex';"
            "  status.innerHTML = '<span>Checking...</span><span></span>';"
            "  try {"
            "    const resp = await fetch('/update', { method: 'POST' });"
            "    const data = await resp.json();"
            "    status.innerHTML = '<span>' + (data.message || '') + '</span><span>' + (data.latest || '') + '</span>';"
            "    if (!data.updating) btn.disabled = false;"
            "  } catch (err) {"
            "    status.innerHTML = '<span>Request failed.</span><span></span>';"
            "    btn.disabled = false;"
            "  }"
            "}"
            "</script>"
            "</body></html>", FIRMWARE_VERSION);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return ESP_OK;
}

static esp_err_t config_page_handler(httpd_req_t *req) {
    return send_config_page(req, NULL);
}

/* Manual OTA trigger for the "Check for updates" button on /wifi.
 * Runs the check synchronously and, if an update is found, kicks off the
 * flash+reboot — so a successful call to this handler never actually
 * returns an HTTP response (the device reboots mid-flash); the client
 * simply loses the connection, which the button's fetch() already treats
 * as a terminal state via its catch branch. */
static esp_err_t update_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");

    if (ota_is_busy()) {
        httpd_resp_send(req,
            "{\"ok\":false,\"updating\":false,\"message\":\"An update check is already running.\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char message[128] = {0};
    char latest[32] = {0};
    ota_check_result_t result = ota_check_and_update(message, sizeof(message));
    ota_get_latest_known_version(latest, sizeof(latest));

    char latest_esc[64];
    char message_esc[256];
    html_escape(latest, latest_esc, sizeof(latest_esc));
    html_escape(message, message_esc, sizeof(message_esc));

    char resp[400];
    bool updating = (result == OTA_CHECK_UPDATE_STARTED);
    bool ok = (result == OTA_CHECK_UP_TO_DATE || result == OTA_CHECK_UPDATE_STARTED || result == OTA_CHECK_UPDATE_AVAILABLE);

    snprintf(resp, sizeof(resp),
        "{\"ok\":%s,\"updating\":%s,\"message\":\"%s\",\"latest\":\"%s\"}",
        ok ? "true" : "false", updating ? "true" : "false", message_esc, latest_esc);

    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* --- Temporary "test connect" event handling, used only while /save is
 * validating credentials before committing them to NVS. Access to the
 * shared Wi-Fi state machine is serialized by wifi_test_mutex, so only
 * one test connection can be in flight at a time and there is no window
 * where both this handler and the permanent one race on the same bits. */
static void wifi_test_event_handler(void* arg, esp_event_base_t event_base,
                                     int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = disconn ? disconn->reason : 0;

        bool is_auth_failure =
            (reason == WIFI_REASON_AUTH_FAIL) ||
            (reason == WIFI_REASON_AUTH_EXPIRE) ||
            (reason == WIFI_REASON_HANDSHAKE_TIMEOUT) ||
            (reason == WIFI_REASON_MIC_FAILURE) ||
            (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) ||
            (reason == WIFI_REASON_ASSOC_NOT_AUTHED);

        if (is_auth_failure) {
            xEventGroupSetBits(wifi_event_group, WIFI_AUTH_FAIL_BIT);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Attempts to associate with the given SSID/password and blocks until
 * success, failure, or timeout. Serialized via wifi_test_mutex so that
 * concurrent HTTP clients hitting /save cannot both drive the Wi-Fi
 * driver's connection state at once, and so a concurrent /wifi scan
 * cannot interleave with the connect/disconnect sequence either. */
static esp_err_t try_wifi_connection(const char *ssid, const char *password, EventBits_t *out_result_bit) {
    if (xSemaphoreTake(wifi_test_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "Another connection attempt is already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_AUTH_FAIL_BIT);

    esp_event_handler_instance_t instance_any_id = NULL;
    esp_event_handler_instance_t instance_got_ip = NULL;

    esp_err_t err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_test_event_handler, NULL, &instance_any_id);
    if (err != ESP_OK) {
        xSemaphoreGive(wifi_test_mutex);
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_test_event_handler, NULL, &instance_got_ip);
    if (err != ESP_OK) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
        xSemaphoreGive(wifi_test_mutex);
        return err;
    }

    wifi_config_t wifi_config = {0};
    size_t max_ssid = sizeof(wifi_config.sta.ssid);
    size_t max_pass = sizeof(wifi_config.sta.password);
    strncpy((char*)wifi_config.sta.ssid, ssid, max_ssid - 1);
    wifi_config.sta.ssid[max_ssid - 1] = '\0';
    strncpy((char*)wifi_config.sta.password, password, max_pass - 1);
    wifi_config.sta.password[max_pass - 1] = '\0';

    /* If either call fails (e.g. driver busy / bad state), don't sit through
     * the full timeout doing nothing — bail out and report a clean error
     * instead of leaving the caller waiting on bits that will never be set. */
    esp_err_t set_cfg_err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_err_t connect_err = (set_cfg_err == ESP_OK) ? esp_wifi_connect() : set_cfg_err;

    if (connect_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi connection attempt: %s", esp_err_to_name(connect_err));
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
        xSemaphoreGive(wifi_test_mutex);
        *out_result_bit = WIFI_FAIL_BIT;
        return ESP_OK; /* still a well-formed "failed to connect" result for the caller */
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_AUTH_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);

    if (bits == 0) {
        esp_wifi_disconnect();
        *out_result_bit = WIFI_FAIL_BIT;
    } else {
        *out_result_bit = bits;
    }

    xSemaphoreGive(wifi_test_mutex);
    return ESP_OK;
}

static esp_err_t save_credentials_handler(httpd_req_t *req) {
    if (req->content_len <= 0 || (size_t)req->content_len > 511) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request size");
        return ESP_FAIL;
    }

    char buf[512];
    int total_received = 0;
    int remaining = req->content_len;

    /* Bound the whole read by wall-clock time, not just per-call timeouts.
     * A client that opens the connection, sends Content-Length, and then
     * stalls (slow-loris style, or just a flaky network) would otherwise
     * keep this HTTP worker task busy in an unbounded retry loop on
     * HTTPD_SOCK_ERR_TIMEOUT, tying up one of the server's limited worker
     * slots indefinitely. */
    const TickType_t recv_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(8000);

    while (remaining > 0) {
        if (xTaskGetTickCount() >= recv_deadline) {
            ESP_LOGW(TAG, "Timed out reading /save request body");
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Request body read timed out");
            return ESP_FAIL;
        }
        int ret = httpd_req_recv(req, buf + total_received, remaining);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Error reading data");
            return ESP_FAIL;
        }
        total_received += ret;
        remaining -= ret;
    }
    buf[total_received] = '\0';

    char raw_ssid[64] = {0};
    char raw_password[128] = {0};

    char *ssid_start = strstr(buf, "ssid=");
    char *pass_start = strstr(buf, "password=");

    if (!ssid_start || !pass_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing required fields");
        return ESP_FAIL;
    }

    ssid_start += strlen("ssid=");
    char *ssid_end = strchr(ssid_start, '&');
    size_t ssid_raw_len = ssid_end ? (size_t)(ssid_end - ssid_start) : strlen(ssid_start);
    if (ssid_raw_len >= sizeof(raw_ssid)) ssid_raw_len = sizeof(raw_ssid) - 1;
    memcpy(raw_ssid, ssid_start, ssid_raw_len);
    raw_ssid[ssid_raw_len] = '\0';

    pass_start += strlen("password=");
    char *pass_end = strchr(pass_start, '&');
    size_t pass_raw_len = pass_end ? (size_t)(pass_end - pass_start) : strlen(pass_start);
    if (pass_raw_len >= sizeof(raw_password)) pass_raw_len = sizeof(raw_password) - 1;
    memcpy(raw_password, pass_start, pass_raw_len);
    raw_password[pass_raw_len] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    url_decode(raw_ssid, ssid, sizeof(ssid));
    url_decode(raw_password, password, sizeof(password));

    httpd_resp_set_type(req, "application/json");

    if (strlen(ssid) == 0) {
        httpd_resp_send(req, "{\"ok\":false,\"message\":\"SSID cannot be empty\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    size_t pass_len_chars = strlen(password);
    if (pass_len_chars > 0 && (pass_len_chars < 8 || pass_len_chars > 63)) {
        httpd_resp_send(req,
            "{\"ok\":false,\"message\":\"Password must be 8-63 characters (or empty for an open network)\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    EventBits_t result_bit = 0;
    esp_err_t err = try_wifi_connection(ssid, password, &result_bit);

    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req,
            "{\"ok\":false,\"message\":\"Another connection attempt is in progress, please wait.\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to attempt Wi-Fi connection: %s", esp_err_to_name(err));
        httpd_resp_send(req, "{\"ok\":false,\"message\":\"Internal error while connecting\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (result_bit & WIFI_AUTH_FAIL_BIT) {
        ESP_LOGW(TAG, "Test connection failed: wrong password for SSID '%s'", ssid);
        blink_error_led();
        httpd_resp_send(req,
            "{\"ok\":false,\"message\":\"Wrong password. Please check it and try again.\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (result_bit & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "Test connection failed: could not reach SSID '%s'", ssid);
        char ssid_esc[132];
        html_escape(ssid, ssid_esc, sizeof(ssid_esc)); /* defensive: this JSON field is client-rendered as text */
        char resp[256];
        snprintf(resp, sizeof(resp),
            "{\"ok\":false,\"message\":\"Could not connect to '%s'. Check the network and try again.\"}", ssid_esc);
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    nvs_handle_t nvs_h;
    err = nvs_open("storage", NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        httpd_resp_send(req, "{\"ok\":false,\"message\":\"Internal storage error\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    err = nvs_set_str(nvs_h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs_h, "password", password);
    if (err == ESP_OK) err = nvs_commit(nvs_h);
    nvs_close(nvs_h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save Wi-Fi data: %s", esp_err_to_name(err));
        httpd_resp_send(req, "{\"ok\":false,\"message\":\"Failed to save credentials\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Connected successfully, saved credentials, rebooting");
    httpd_resp_send(req, "{\"ok\":true,\"message\":\"Connected! Rebooting...\"}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

    return ESP_OK;
}

static const char* reset_page_template =
    "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<meta charset='utf-8'><title>Factory Reset</title>"
    "<style>%s</style></head><body>"
    "<div class='wrap'>"
    "<p class='eyebrow'>Smart Lamp</p>"
    "<h1>Factory reset</h1>"
    "<p class='subtitle'>Erases saved Wi&#8209;Fi credentials. This cannot be undone; the lamp will restart in setup mode.</p>"
    "<form action='/reset' method='POST'>"
    "<button class='btn btn-danger' type='submit'>Yes, erase and reset</button>"
    "</form>"
    "<hr>"
    "<p class='footer-note'><a href='/'>&larr; Cancel, go back</a></p>"
    "</div>"
    "</body></html>";

static esp_err_t reset_page_handler(httpd_req_t *req) {
    /* Sized at runtime against the actual template + CSS length, same
     * reasoning as root_handler: avoids -Werror=format-truncation and
     * stays correct if either string is edited later. */
    size_t page_capacity = strlen(reset_page_template) + strlen(shared_style_css) + 64;
    char *page = malloc(page_capacity);
    if (!page) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int n = snprintf(page, page_capacity, reset_page_template, shared_style_css);

    if (n < 0 || (size_t)n >= page_capacity) {
        free(page);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return ESP_OK;
}

static esp_err_t reset_action_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Resetting to factory settings... Rebooting.", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));

    led_state = false;
    gpio_set_level(LED_GPIO, 1);

    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
    }
    esp_restart();
    return ESP_OK;
}

void start_web_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* Let the server evict the oldest connection instead of refusing new
     * ones outright when all sockets are busy — a client that never
     * closes its socket shouldn't be able to lock everyone else out. */
    config.lru_purge_enable = true;
    /* /update can legitimately take a while (GitHub API round-trip +
     * potential multi-hundred-KB firmware download), so give handlers
     * more headroom than the httpd default. */
    config.stack_size = 8192;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t toggle_uri = { .uri = "/toggle", .method = HTTP_POST, .handler = toggle_handler };
        httpd_register_uri_handler(server, &toggle_uri);

        httpd_uri_t wifi_uri = { .uri = "/wifi", .method = HTTP_GET, .handler = config_page_handler };
        httpd_register_uri_handler(server, &wifi_uri);

        httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = save_credentials_handler };
        httpd_register_uri_handler(server, &save_uri);

        httpd_uri_t reset_get_uri = { .uri = "/reset", .method = HTTP_GET, .handler = reset_page_handler };
        httpd_register_uri_handler(server, &reset_get_uri);

        httpd_uri_t reset_post_uri = { .uri = "/reset", .method = HTTP_POST, .handler = reset_action_handler };
        httpd_register_uri_handler(server, &reset_post_uri);

        httpd_uri_t update_uri = { .uri = "/update", .method = HTTP_POST, .handler = update_handler };
        httpd_register_uri_handler(server, &update_uri);
    } else {
        ESP_LOGE(TAG, "Failed to start control web server, restarting to retry");
        /* A lamp with no web server and no way to reach it is exactly the
         * "have to physically open it" scenario we're trying to avoid.
         * A clean restart at least gives the boot sequence another shot. */
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

void start_config_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t config_uri = { .uri = "/", .method = HTTP_GET, .handler = config_page_handler };
        httpd_register_uri_handler(server, &config_uri);

        httpd_uri_t save_uri = { .uri = "/save", .method = HTTP_POST, .handler = save_credentials_handler };
        httpd_register_uri_handler(server, &save_uri);
        /* Deliberately no /update route here: OTA needs an internet-routed
         * STA connection, which doesn't exist yet in AP-only setup mode. */
    } else {
        ESP_LOGE(TAG, "Failed to start configuration server, restarting to retry");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

static void start_mdns_service(void) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("light");
    mdns_instance_name_set("light-setup");

    mdns_txt_item_t serviceTxtData[] = {
        {"board", "esp32"}
    };
    mdns_service_add("ESP32 Web Server", "_http", "_tcp", 80, serviceTxtData, 1);
    ESP_LOGI(TAG, "mDNS started: http://light.local");
}

/* Minimal captive-portal DNS responder: answers every A-record query
 * with the AP's own IP. Validates that the packet actually contains a
 * well-formed question section before touching it, so malformed or
 * truncated UDP packets can't cause an out-of-bounds copy. */
static void dns_server_task(void *pvParameters) {
    uint8_t rx_buffer[DNS_MAX_PACKET_SIZE];
    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL || esp_netif_get_ip_info(ap_netif, &ip_info) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP IP info, DNS server stopped");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server started, redirecting all queries to " IPSTR, IP2STR(&ip_info.ip));

    /* If recvfrom() keeps erroring (e.g. a transient socket/network
     * issue), avoid a tight busy-loop that starves other tasks at the
     * same priority and can trip the idle-task watchdog. A short delay
     * on error costs nothing on the happy path, where recvfrom blocks. */
    while (1) {
        client_addr_len = sizeof(client_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                            (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < 0) {
            ESP_LOGW(TAG, "DNS recvfrom error, retrying");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (len < DNS_HEADER_SIZE + 5) {
            /* Too short to contain a header plus a minimal question
             * (1 length byte + 1 root byte + QTYPE(2) + QCLASS(2)). */
            continue;
        }

        /* Walk the QNAME looking for the terminating zero length byte.
         * Bound the search strictly by len so we never read past what
         * recvfrom actually delivered. */
        int qname_end = DNS_HEADER_SIZE;
        bool found_terminator = false;
        while (qname_end < len) {
            if (rx_buffer[qname_end] == 0) {
                found_terminator = true;
                break;
            }
            qname_end++;
        }
        if (!found_terminator) continue;

        qname_end += 1 /* terminating zero */ + 4 /* QTYPE + QCLASS */;
        if (qname_end > len) continue;

        int question_len = qname_end; /* header + full question section */
        if (question_len + DNS_ANSWER_SIZE > (int)sizeof(rx_buffer)) continue;

        uint8_t response[DNS_MAX_PACKET_SIZE];
        memcpy(response, rx_buffer, question_len);

        response[DNS_FLAGS_HI_OFFSET] = DNS_FLAGS_HI_RESPONSE;
        response[DNS_FLAGS_LO_OFFSET] = DNS_FLAGS_LO_RESPONSE;
        response[DNS_QDCOUNT_HI_OFFSET] = 0x00; response[DNS_QDCOUNT_LO_OFFSET] = 0x01;
        response[DNS_ANCOUNT_HI_OFFSET] = 0x00; response[DNS_ANCOUNT_LO_OFFSET] = 0x01;
        response[DNS_NSCOUNT_HI_OFFSET] = 0x00; response[DNS_NSCOUNT_LO_OFFSET] = 0x00;

        uint8_t answer[DNS_ANSWER_SIZE] = {
            0xC0, 0x0C,             /* name: pointer to offset 12 (the question's QNAME) */
            0x00, 0x01,             /* TYPE: A */
            0x00, 0x01,             /* CLASS: IN */
            0x00, 0x00, 0x00, 0x3C, /* TTL: 60s */
            0x00, 0x04              /* RDLENGTH: 4 bytes */
            /* remaining 4 bytes: RDATA, filled in below */
        };
        memcpy(answer + 12, &ip_info.ip.addr, 4);

        int resp_len = question_len;
        memcpy(response + resp_len, answer, sizeof(answer));
        resp_len += sizeof(answer);

        sendto(sock, response, resp_len, 0, (struct sockaddr *)&client_addr, client_addr_len);
    }

    close(sock);
    vTaskDelete(NULL);
}

static void start_dns_server(void) {
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle);
}

static void blink_error_led(void) {
    for (int i = 0; i < 5; i++) {
        gpio_set_level(LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

/* Permanent Wi-Fi event handler, active only for the initial STA
 * connection attempt at boot (using saved credentials). Once we enter
 * AP+STA setup mode, in_setup_mode is set and this handler's disconnect
 * branch is bypassed so it can't race with wifi_test_event_handler. */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (in_setup_mode) return; /* let wifi_test_event_handler own this */

        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = disconn ? disconn->reason : 0;

        bool is_auth_failure =
            (reason == WIFI_REASON_AUTH_FAIL) ||
            (reason == WIFI_REASON_AUTH_EXPIRE) ||
            (reason == WIFI_REASON_HANDSHAKE_TIMEOUT) ||
            (reason == WIFI_REASON_MIC_FAILURE) ||
            (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) ||
            (reason == WIFI_REASON_ASSOC_NOT_AUTHED);

        if (is_auth_failure) {
            ESP_LOGE(TAG, "Wi-Fi disconnected: wrong password / auth failure (reason=%d)", reason);
            wifi_retry_count = 0;
            blink_error_led();
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT | WIFI_AUTH_FAIL_BIT);
            return;
        }

        ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d)", reason);

        if (wifi_retry_count < WIFI_CONNECT_MAX_RETRIES) {
            wifi_retry_count++;
            ESP_LOGI(TAG, "Retrying connection (%d/%d)...", wifi_retry_count, WIFI_CONNECT_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP address");
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Watches the running HTTP server handle. If it were ever left NULL
 * outside of our own controlled restart paths, the lamp would still
 * power the LED but be completely unreachable over the network — the
 * exact "have to open the case" scenario. Cheap, low-priority safety
 * net on top of the explicit restart-on-failure calls in
 * start_web_server()/start_config_server(). */
static void httpd_health_check_task(void *pvParameters) {
    const TickType_t check_interval = pdMS_TO_TICKS(30000);
    for (;;) {
        vTaskDelay(check_interval);
        if (server == NULL) {
            ESP_LOGE(TAG, "HTTP server handle is NULL during health check, restarting");
            esp_restart();
        }
    }
}

void app_main(void) {
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 1);

    wifi_test_mutex = xSemaphoreCreateMutex();
    if (wifi_test_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi test mutex, restarting");
        esp_restart();
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_event_group = xEventGroupCreate();

    nvs_handle_t nvs_h;
    char ssid[33] = {0};
    char password[65] = {0};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(password);
    bool have_credentials = false;

    if (nvs_open("storage", NVS_READONLY, &nvs_h) == ESP_OK) {
        esp_err_t err_s = nvs_get_str(nvs_h, "ssid", ssid, &ssid_len);
        esp_err_t err_p = nvs_get_str(nvs_h, "password", password, &pass_len);
        nvs_close(nvs_h);
        have_credentials = (err_s == ESP_OK && err_p == ESP_OK && strlen(ssid) > 0);
    }

    if (have_credentials) {
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                              &wifi_event_handler, NULL, &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                              &wifi_event_handler, NULL, &instance_got_ip));

        wifi_config_t wifi_config = {0};
        size_t max_ssid = sizeof(wifi_config.sta.ssid);
        size_t max_pass = sizeof(wifi_config.sta.password);
        strncpy((char*)wifi_config.sta.ssid, ssid, max_ssid - 1);
        wifi_config.sta.ssid[max_ssid - 1] = '\0';
        strncpy((char*)wifi_config.sta.password, password, max_pass - 1);
        wifi_config.sta.password[max_pass - 1] = '\0';

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Connecting to Wi-Fi network...");

        EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT | WIFI_AUTH_FAIL_BIT,
                                                pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connection successful");
            start_mdns_service();
            start_web_server();
            xTaskCreate(httpd_health_check_task, "httpd_health", 2048, NULL, 3, NULL);
            /* We've now proven Wi-Fi connects and the web server comes up
             * on this image -- confirm it so the bootloader doesn't roll
             * back to the previous firmware on the next reboot (only
             * matters if app rollback is enabled in sdkconfig; harmless
             * no-op otherwise). Do this before starting the auto-update
             * task so a freshly-flashed image is confirmed before it
             * might attempt yet another OTA. */
            ota_confirm_running_app();
            ota_start_auto_check_task();
            return;
        }

        if (bits & WIFI_AUTH_FAIL_BIT) {
            ESP_LOGE(TAG, "Saved Wi-Fi password appears to be incorrect. Entering setup mode.");
        } else {
            ESP_LOGW(TAG, "Failed to connect within %d ms, entering setup mode", WIFI_CONNECT_TIMEOUT_MS);
        }

        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_deinit());
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
        esp_netif_destroy_default_wifi(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
    }

    ESP_LOGI(TAG, "Starting AP mode for setup...");
    in_setup_mode = true; /* from here on, wifi_test_event_handler owns disconnect handling */

    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = "light-setup",
            .ssid_len = strlen("light-setup"),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_mdns_service();
    start_dns_server();
    start_config_server();
    xTaskCreate(httpd_health_check_task, "httpd_health", 2048, NULL, 3, NULL);
}