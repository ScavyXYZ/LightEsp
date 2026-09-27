#include "ota_update.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "ota_update";

/* We use ESP-IDF's built-in certificate bundle (esp_crt_bundle) instead of a
 * single hand-embedded root CA PEM. It ships a curated set of public root
 * CAs (the same trust set curl/Mozilla use) compiled into the firmware, and
 * esp_http_client / esp_https_ota pick the right one automatically for
 * whatever host they connect to (api.github.com, and the
 * objects.githubusercontent.com host that release asset downloads redirect
 * to). This avoids maintaining/copy-pasting a raw PEM file by hand, which is
 * error-prone. Requires CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y (default on in
 * recent ESP-IDF; see OTA_SETUP.md). */

#define OTA_AUTO_CHECK_INTERVAL_MS (24ULL * 60 * 60 * 1000) /* once a day */
#define OTA_HTTP_TIMEOUT_MS        15000
/* GitHub's releases/latest JSON includes a full record (multiple long
 * URLs, uploader info, timestamps) for every asset attached to the
 * release, not just the firmware .bin. A release with just a few extra
 * assets (source archives, checksums, etc.) can easily exceed 4-6KB;
 * 4096 was routinely truncating real releases and silently failing the
 * update check every single time. 16KB comfortably covers a release with
 * a handful of assets while still bounding worst-case heap usage. */
#define OTA_MANIFEST_BUF_SIZE      16384
#define OTA_MAX_VERSION_LEN        32
#define OTA_MAX_URL_LEN            512

/* FIX (#4, #12): ota_busy is now a proper critical-section-protected flag
 * instead of a bare volatile bool.  The previous code set it outside the
 * mutex in one branch and read it without any lock in ota_is_busy(), which
 * could leave it stuck at true forever if a task got preempted at the wrong
 * moment.  We keep the mutex only for the state snapshot itself (it must NOT
 * be held across the long flash/HTTP operations), and use a dedicated
 * "check in progress" flag protected by a short critical section. */
static SemaphoreHandle_t ota_mutex = NULL;
static portMUX_TYPE ota_state_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool ota_busy = false;

/* FIX (#12): latest_known_version is written by the OTA task and read by
 * the HTTP handler; guard it with its own mutex so a reader never sees a
 * half-written string. */
static SemaphoreHandle_t version_mutex = NULL;
static char latest_known_version[OTA_MAX_VERSION_LEN] = {0};

/* --- tiny helper: growable buffer for the HTTP GET body --- */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} dyn_buf_t;

static esp_err_t dyn_buf_append(dyn_buf_t *b, const char *chunk, size_t chunk_len) {
    if (b->len + chunk_len + 1 > b->cap) {
        size_t new_cap = b->cap == 0 ? 1024 : b->cap;
        while (new_cap < b->len + chunk_len + 1) new_cap *= 2;
        if (new_cap > OTA_MANIFEST_BUF_SIZE) {
            ESP_LOGW(TAG, "Manifest JSON exceeded %d bytes (need >=%u); "
                     "raise OTA_MANIFEST_BUF_SIZE or trim release assets",
                     OTA_MANIFEST_BUF_SIZE, (unsigned)(b->len + chunk_len + 1));
            return ESP_ERR_NO_MEM;
        }
        char *new_data = realloc(b->data, new_cap);
        if (!new_data) return ESP_ERR_NO_MEM;
        b->data = new_data;
        b->cap = new_cap;
    }
    memcpy(b->data + b->len, chunk, chunk_len);
    b->len += chunk_len;
    b->data[b->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    dyn_buf_t *buf = (dyn_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf) {
        if (dyn_buf_append(buf, (const char *)evt->data, evt->data_len) != ESP_OK) {
            ESP_LOGW(TAG, "Manifest response too large, truncating");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/* Fetches https://api.github.com/repos/OWNER/REPO/releases/latest and
 * extracts tag_name + the browser_download_url of OTA_FIRMWARE_ASSET_NAME.
 * Returns ESP_OK only if both were found. */
static esp_err_t fetch_release_manifest(char *out_version, size_t out_version_size,
                                         char *out_asset_url, size_t out_asset_url_size) {
    dyn_buf_t buf = {0};
    esp_err_t ret = ESP_FAIL;

    esp_http_client_config_t config = {
        .url = "https://api.github.com/repos/" OTA_GITHUB_OWNER "/" OTA_GITHUB_REPO "/releases/latest",
        .event_handler = http_event_handler,
        .user_data = &buf,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    /* GitHub API requires a User-Agent header or it rejects the request. */
    esp_http_client_set_header(client, "User-Agent", "LightEsp-OTA");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GitHub API request failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "GitHub API returned HTTP %d", status);
        goto cleanup;
    }

    if (!buf.data) {
        ESP_LOGE(TAG, "Empty response from GitHub API");
        goto cleanup;
    }

    cJSON *root = cJSON_Parse(buf.data);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse release manifest JSON");
        goto cleanup;
    }

    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (!cJSON_IsString(tag) || !tag->valuestring) {
        ESP_LOGE(TAG, "Manifest missing tag_name");
        cJSON_Delete(root);
        goto cleanup;
    }
    strncpy(out_version, tag->valuestring, out_version_size - 1);
    out_version[out_version_size - 1] = '\0';

    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    bool found_asset = false;
    if (cJSON_IsArray(assets)) {
        const cJSON *asset;
        cJSON_ArrayForEach(asset, assets) {
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
            const cJSON *url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
            if (cJSON_IsString(name) && name->valuestring &&
                strcmp(name->valuestring, OTA_FIRMWARE_ASSET_NAME) == 0 &&
                cJSON_IsString(url) && url->valuestring) {
                strncpy(out_asset_url, url->valuestring, out_asset_url_size - 1);
                out_asset_url[out_asset_url_size - 1] = '\0';
                found_asset = true;
                break;
            }
        }
    }
    cJSON_Delete(root);

    if (!found_asset) {
        ESP_LOGE(TAG, "Release '%s' has no asset named '%s'", out_version, OTA_FIRMWARE_ASSET_NAME);
        goto cleanup;
    }

    ret = ESP_OK;

cleanup:
    esp_http_client_cleanup(client);
    if (buf.data) free(buf.data);
    return ret;
}

/* Very small, permissive semver-ish comparison. Versions are expected as
 * "vMAJOR.MINOR.PATCH" (leading 'v' optional). Anything it can't parse is
 * treated conservatively as "not newer", so a malformed tag never forces
 * an unwanted flash. */
static bool remote_version_is_newer(const char *current, const char *remote) {
    unsigned cur[3] = {0, 0, 0};
    unsigned rem[3] = {0, 0, 0};

    const char *c = current;
    if (*c == 'v' || *c == 'V') c++;
    const char *r = remote;
    if (*r == 'v' || *r == 'V') r++;

    if (sscanf(c, "%u.%u.%u", &cur[0], &cur[1], &cur[2]) < 1) {
        ESP_LOGW(TAG, "Could not parse current version '%s' as semver, treating as not-newer", current);
        return false;
    }
    if (sscanf(r, "%u.%u.%u", &rem[0], &rem[1], &rem[2]) < 1) {
        ESP_LOGW(TAG, "Could not parse remote tag '%s' as semver, treating as not-newer", remote);
        return false;
    }

    for (int i = 0; i < 3; i++) {
        if (rem[i] > cur[i]) return true;
        if (rem[i] < cur[i]) return false;
    }
    return false; /* equal */
}

static esp_err_t perform_ota_flash(const char *firmware_url) {
    esp_http_client_config_t http_config = {
        .url = firmware_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        /* GitHub's release download redirects (github.com ->
         * objects.githubusercontent.com) carry a long signed Location
         * header (often 1-2KB with the access token/query params), which
         * overflows esp_http_client's default header buffer and fails
         * with "Out of buffer" before the redirect can even be followed.
         * Bump both buffers well past that; a few KB is cheap against the
         * heap headroom this task already has (8KB stack via task create). */
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    ESP_LOGI(TAG, "Starting OTA flash from %s", firmware_url);
    esp_err_t err = esp_https_ota(&ota_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA flash successful, rebooting");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "OTA flash failed: %s", esp_err_to_name(err));
    return err;
}

/* FIX (#4): atomic-ish busy flag accessors.  We use a portMUX critical
 * section (very cheap) instead of taking the OTA mutex, because the mutex
 * must not be held for the long duration of a flash — that would make
 * ota_is_busy() block for minutes.  A critical section here is enough to
 * give readers a consistent view. */
static bool ota_busy_get(void) {
    portENTER_CRITICAL(&ota_state_lock);
    bool v = ota_busy;
    portEXIT_CRITICAL(&ota_state_lock);
    return v;
}

static void ota_busy_set(bool v) {
    portENTER_CRITICAL(&ota_state_lock);
    ota_busy = v;
    portEXIT_CRITICAL(&ota_state_lock);
}

static void ota_set_latest_version(const char *v) {
    if (!version_mutex) return;
    if (xSemaphoreTake(version_mutex, portMAX_DELAY) == pdTRUE) {
        strncpy(latest_known_version, v, sizeof(latest_known_version) - 1);
        latest_known_version[sizeof(latest_known_version) - 1] = '\0';
        xSemaphoreGive(version_mutex);
    }
}

static ota_check_result_t do_check_and_maybe_update(bool apply_update, char *out_message, size_t out_message_size) {
    /* We serialize *entire* check+update operations behind ota_mutex so two
     * concurrent callers can never both start a flash.  The mutex is only
     * released across the actual esp_https_ota() call... but only after we
     * have set the busy flag under the critical section, so ota_is_busy()
     * still reports true during the flash. */
    if (xSemaphoreTake(ota_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        if (out_message) snprintf(out_message, out_message_size, "An update check is already in progress.");
        return OTA_CHECK_ERROR;
    }

    /* If some other task is mid-flash, don't even try. */
    if (ota_busy_get()) {
        xSemaphoreGive(ota_mutex);
        if (out_message) snprintf(out_message, out_message_size, "An update is already in progress.");
        return OTA_CHECK_ERROR;
    }
    ota_busy_set(true);

    char remote_version[OTA_MAX_VERSION_LEN] = {0};
    char asset_url[OTA_MAX_URL_LEN] = {0};
    ota_check_result_t result;

    esp_err_t err = fetch_release_manifest(remote_version, sizeof(remote_version), asset_url, sizeof(asset_url));
    if (err != ESP_OK) {
        if (out_message) snprintf(out_message, out_message_size, "Could not reach GitHub or parse the release info.");
        result = OTA_CHECK_ERROR;
        goto done;
    }

    ota_set_latest_version(remote_version);

    if (!remote_version_is_newer(FIRMWARE_VERSION, remote_version)) {
        ESP_LOGI(TAG, "Firmware up to date (current=%s, latest=%s)", FIRMWARE_VERSION, remote_version);
        if (out_message) snprintf(out_message, out_message_size, "Already up to date (%s).", FIRMWARE_VERSION);
        result = OTA_CHECK_UP_TO_DATE;
        goto done;
    }

    ESP_LOGI(TAG, "Update available: %s -> %s", FIRMWARE_VERSION, remote_version);

    if (!apply_update) {
        if (out_message) snprintf(out_message, out_message_size, "Update %s is available.", remote_version);
        result = OTA_CHECK_UPDATE_AVAILABLE;
        goto done;
    }

    if (out_message) snprintf(out_message, out_message_size, "Downloading and installing %s...", remote_version);
    result = OTA_CHECK_UPDATE_STARTED;

    /* FIX (#4): release the mutex *only* for the long flash, but the busy
     * flag (set above under the critical section) stays true.  Anyone who
     * calls do_check_and_maybe_update() in the meantime will take the
     * mutex, immediately see ota_busy_get()==true, and bail out cleanly. */
    xSemaphoreGive(ota_mutex);

    err = perform_ota_flash(asset_url);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        /* unreachable */
    }

    /* Flash failed: re-acquire the mutex to clear busy state consistently. */
    xSemaphoreTake(ota_mutex, portMAX_DELAY);
    if (out_message) snprintf(out_message, out_message_size, "Update download/flash failed, kept running current firmware.");
    result = OTA_CHECK_ERROR;

done:
    ota_busy_set(false);
    xSemaphoreGive(ota_mutex);
    return result;
}

ota_check_result_t ota_check_and_update(char *out_message, size_t out_message_size) {
    return do_check_and_maybe_update(true, out_message, out_message_size);
}

bool ota_is_busy(void) {
    return ota_busy_get();
}

void ota_get_latest_known_version(char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!version_mutex) return;
    if (xSemaphoreTake(version_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(out, latest_known_version, out_size - 1);
        out[out_size - 1] = '\0';
        xSemaphoreGive(version_mutex);
    }
}

static void ota_auto_check_task(void *pvParameters) {
    /* Give the network a moment to settle right after boot before the
     * first check, then fall into the regular daily cadence. */
    vTaskDelay(pdMS_TO_TICKS(30000));

    for (;;) {
        char msg[128];
        ESP_LOGI(TAG, "Running scheduled update check");
        do_check_and_maybe_update(true, msg, sizeof(msg));
        ESP_LOGI(TAG, "Scheduled update check result: %s", msg);

        vTaskDelay(pdMS_TO_TICKS(OTA_AUTO_CHECK_INTERVAL_MS));
    }
}

void ota_start_auto_check_task(void) {
    if (ota_mutex == NULL) {
        ota_mutex = xSemaphoreCreateMutex();
    }
    if (version_mutex == NULL) {
        version_mutex = xSemaphoreCreateMutex();
    }
    /* FIX (#9): cJSON_Parse on a 16KB manifest plus TLS handshake can be
     * stack-hungry; 8192 was marginal.  Bump to 12288 for headroom. */
    xTaskCreate(ota_auto_check_task, "ota_auto_check", 12288, NULL, 3, NULL);
}

void ota_confirm_running_app(void) {
    /* If CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is on, a freshly-flashed OTA
     * image boots in "pending verify" state and the bootloader will revert
     * to the previous firmware on the *next* reboot unless something in the
     * new image explicitly confirms it's good. This call is the standard
     * ESP-IDF confirmation; it's a harmless no-op if rollback support isn't
     * enabled, so it's always safe to call once, early, after we know the
     * app has come up cleanly (e.g. once Wi-Fi/web server are up). */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Marked running app as valid, cancelled pending rollback");
        } else {
            ESP_LOGW(TAG, "Failed to confirm running app: %s", esp_err_to_name(err));
        }
    }
}