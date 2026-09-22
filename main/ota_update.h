#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Bump this with every release. Must exactly match the "tag_name" of the
 * GitHub release you publish (e.g. tag "v1.0.3" -> FIRMWARE_VERSION "v1.0.3"). */
#define FIRMWARE_VERSION "v1.0.3"

/* GitHub repo that hosts releases (owner/repo, no scheme). */
#define OTA_GITHUB_OWNER "ScavyXYZ"
#define OTA_GITHUB_REPO  "LightEsp"

/* Name of the .bin asset attached to each GitHub release. */
#define OTA_FIRMWARE_ASSET_NAME "LightEsp.bin"

typedef enum {
    OTA_CHECK_UP_TO_DATE,
    OTA_CHECK_UPDATE_AVAILABLE,
    OTA_CHECK_UPDATE_STARTED,   /* update found and flashing began (only returned by ota_check_and_update) */
    OTA_CHECK_ERROR
} ota_check_result_t;

/* Starts the background task that checks GitHub Releases once every
 * OTA_AUTO_CHECK_INTERVAL_MS and applies an update automatically if found.
 * Safe to call once, after Wi-Fi (station mode) is connected. */
void ota_start_auto_check_task(void);

/* Synchronous manual check+update, meant to be called from the /update
 * HTTP handler. Blocks until the check (and, if applicable, the OTA
 * flash + reboot) is done. On success this function does not return
 * (device reboots); it only returns if no update was found or on error. */
ota_check_result_t ota_check_and_update(char *out_message, size_t out_message_size);

/* True while a check or flash is already in progress, so callers (like
 * the HTTP handler) can avoid starting two overlapping OTA attempts. */
bool ota_is_busy(void);

/* Latest version string seen on GitHub during the last check (may be
 * empty if no check has completed yet). Thread-safe snapshot copy. */
void ota_get_latest_known_version(char *out, size_t out_size);

/* Confirms the currently running app image is good, cancelling any
 * pending bootloader rollback. Safe no-op if app rollback isn't enabled
 * in sdkconfig. Call once, early, after the app has come up cleanly
 * (e.g. right after Wi-Fi connects and the web server starts) -- ideally
 * before ota_start_auto_check_task(), so a freshly-flashed image is
 * confirmed before it might attempt another OTA. */
void ota_confirm_running_app(void);