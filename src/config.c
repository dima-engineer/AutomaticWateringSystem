#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "config";

#define NS  "watering"   /* NVS namespace — all keys live under this name */

/* Defaults used on first boot or if NVS is erased */
static const watering_config_t DEFAULTS = {
    .threshold_pct    = 40,
    .raw_dry          = 3000,
    .raw_wet          = 1000,
    .pump_duration_ms = 5000,
    .pump_cooldown_ms = 10000,
    .check_interval_ms = 10000,
};

watering_config_t g_config;

/* Helper: read one int32 from NVS; use def if key doesn't exist yet */
static void load_i32(nvs_handle_t h, const char *key, int *dst, int def)
{
    int32_t v;
    *dst = (nvs_get_i32(h, key, &v) == ESP_OK) ? (int)v : def;
}

void config_init(void)
{
    /*
     * nvs_flash_init() must be called once before any NVS access.
     * ESP_ERR_NVS_NO_FREE_PAGES / NEW_VERSION_FOUND → NVS partition is
     * corrupted or was reformatted; erase and reinitialise.
     */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition invalid — erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved config — using defaults");
        g_config = DEFAULTS;
        return;
    }

    load_i32(h, "threshold",  &g_config.threshold_pct,     DEFAULTS.threshold_pct);
    load_i32(h, "raw_dry",    &g_config.raw_dry,           DEFAULTS.raw_dry);
    load_i32(h, "raw_wet",    &g_config.raw_wet,           DEFAULTS.raw_wet);
    load_i32(h, "pump_dur",   &g_config.pump_duration_ms,  DEFAULTS.pump_duration_ms);
    load_i32(h, "cooldown",   &g_config.pump_cooldown_ms,  DEFAULTS.pump_cooldown_ms);
    load_i32(h, "check_int",  &g_config.check_interval_ms, DEFAULTS.check_interval_ms);

    nvs_close(h);
    ESP_LOGI(TAG, "Config loaded from NVS");
}

void config_save(void)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));

    nvs_set_i32(h, "threshold",  g_config.threshold_pct);
    nvs_set_i32(h, "raw_dry",    g_config.raw_dry);
    nvs_set_i32(h, "raw_wet",    g_config.raw_wet);
    nvs_set_i32(h, "pump_dur",   g_config.pump_duration_ms);
    nvs_set_i32(h, "cooldown",   g_config.pump_cooldown_ms);
    nvs_set_i32(h, "check_int",  g_config.check_interval_ms);

    ESP_ERROR_CHECK(nvs_commit(h));   /* flush write buffer to flash */
    nvs_close(h);
    ESP_LOGI(TAG, "Config saved to NVS");
}

void config_print(void)
{
    ESP_LOGI(TAG, "threshold=%d%%  raw_dry=%d  raw_wet=%d  pump=%dms  cooldown=%dms  check=%dms",
             g_config.threshold_pct,
             g_config.raw_dry,
             g_config.raw_wet,
             g_config.pump_duration_ms,
             g_config.pump_cooldown_ms,
             g_config.check_interval_ms);
}
