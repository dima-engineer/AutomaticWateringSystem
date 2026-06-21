#include "ha_mqtt.h"

#if defined(USE_HA_MQTT) && USE_HA_MQTT

#ifndef WIFI_SSID
#error "Set WIFI_SSID in secrets.ini build_flags"
#endif
#ifndef WIFI_PASSWORD
#error "Set WIFI_PASSWORD in secrets.ini build_flags"
#endif
#ifndef MQTT_BROKER_URL
#error "Set MQTT_BROKER_URL in secrets.ini build_flags"
#endif

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ha";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MQTT_CONNECTED_BIT  BIT0

#define WIFI_TIMEOUT_MS   30000
#define MQTT_TIMEOUT_MS   10000
#define MQTT_FLUSH_MS     10000

/*
 * HA MQTT discovery — four entities, all under one "Auto Watering" device.
 *
 * Discovery topics (retained, QoS 0):
 *   homeassistant/sensor/watering/moisture/config
 *   homeassistant/sensor/watering/battery/config
 *   homeassistant/binary_sensor/watering/water_level/config
 *   homeassistant/binary_sensor/watering/pump/config
 *
 * State topics (not retained, QoS 0):
 *   watering/moisture   — integer percent
 *   watering/battery_mv — integer mV
 *   watering/water_ok   — "ON" / "OFF"
 *   watering/pump       — "ON" / "OFF"
 */
#define T_MOISTURE  "watering/moisture"
#define T_BATTERY   "watering/battery_mv"
#define T_WATER_OK  "watering/water_ok"
#define T_PUMP      "watering/pump"

#define DEVICE_JSON \
    "\"device\":{" \
        "\"identifiers\":[\"watering_esp32\"]," \
        "\"name\":\"Auto Watering\"," \
        "\"model\":\"ESP32-C3 Super Mini\"" \
    "}"

static bool s_initialized = false;
static EventGroupHandle_t s_wifi_eg;
static EventGroupHandle_t s_mqtt_eg;

/* ── Event handlers ──────────────────────────────────────────────────────── */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
}

static void on_mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if ((esp_mqtt_event_id_t)id == MQTT_EVENT_CONNECTED)
        xEventGroupSetBits(s_mqtt_eg, MQTT_CONNECTED_BIT);
}

/* ── Init (once per boot) ────────────────────────────────────────────────── */

void ha_mqtt_init(void)
{
    if (s_initialized) return;
    s_initialized = true;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  /* keep radio on — improves connection reliability */
}

/* ── Wi-Fi ───────────────────────────────────────────────────────────────── */

static bool wifi_connect(void)
{
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strlcpy((char *)wc.sta.ssid,     WIFI_SSID,     sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    for (int attempt = 1; attempt <= 3; attempt++) {
        s_wifi_eg = xEventGroupCreate();

        esp_event_handler_instance_t h_wifi, h_ip;
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, &h_wifi);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, &h_ip);

        esp_wifi_connect();

        EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
            pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, h_wifi);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, h_ip);
        vEventGroupDelete(s_wifi_eg);

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Wi-Fi connected (attempt %d)", attempt);
            return true;
        }
        ESP_LOGW(TAG, "Wi-Fi attempt %d failed", attempt);
        /* Disconnect before retry — clears both L2 state and stalled DHCP */
        if (attempt < 3) {
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    return false;
}

/* ── HA discovery payloads ───────────────────────────────────────────────── */

static void publish_discovery(esp_mqtt_client_handle_t c)
{
    char buf[384];

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Soil Moisture\","
        "\"state_topic\":\"" T_MOISTURE "\","
        "\"unit_of_measurement\":\"%%\","
        "\"device_class\":\"moisture\","
        "\"state_class\":\"measurement\","
        "\"force_update\":true,"
        "\"unique_id\":\"watering_moisture\","
        DEVICE_JSON "}");
    esp_mqtt_client_publish(c, "homeassistant/sensor/watering/moisture/config",
                            buf, 0, 0, 1);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Battery\","
        "\"state_topic\":\"" T_BATTERY "\","
        "\"unit_of_measurement\":\"mV\","
        "\"device_class\":\"voltage\","
        "\"state_class\":\"measurement\","
        "\"force_update\":true,"
        "\"unique_id\":\"watering_battery\","
        DEVICE_JSON "}");
    esp_mqtt_client_publish(c, "homeassistant/sensor/watering/battery/config",
                            buf, 0, 0, 1);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Water Level\","
        "\"state_topic\":\"" T_WATER_OK "\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"device_class\":\"moisture\","
        "\"force_update\":true,"
        "\"unique_id\":\"watering_water_level\","
        DEVICE_JSON "}");
    esp_mqtt_client_publish(c, "homeassistant/binary_sensor/watering/water_level/config",
                            buf, 0, 0, 1);

    snprintf(buf, sizeof(buf),
        "{\"name\":\"Pump\","
        "\"state_topic\":\"" T_PUMP "\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"device_class\":\"running\","
        "\"force_update\":true,"
        "\"unique_id\":\"watering_pump\","
        DEVICE_JSON "}");
    esp_mqtt_client_publish(c, "homeassistant/binary_sensor/watering/pump/config",
                            buf, 0, 0, 1);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void ha_mqtt_publish(int moisture_pct, bool water_ok, int battery_mv, bool pump_activated)
{
    if (!wifi_connect()) return;

    s_mqtt_eg = xEventGroupCreate();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URL,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_mqtt_event, NULL);
    esp_mqtt_client_start(client);

    EventBits_t bits = xEventGroupWaitBits(s_mqtt_eg, MQTT_CONNECTED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(MQTT_TIMEOUT_MS));
    vEventGroupDelete(s_mqtt_eg);

    if (!(bits & MQTT_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "MQTT connect failed");
        goto cleanup;
    }

    publish_discovery(client);

    char val[16];
    snprintf(val, sizeof(val), "%d", moisture_pct);
    esp_mqtt_client_publish(client, T_MOISTURE, val, 0, 0, 0);

    snprintf(val, sizeof(val), "%d", battery_mv);
    esp_mqtt_client_publish(client, T_BATTERY, val, 0, 0, 0);

    esp_mqtt_client_publish(client, T_WATER_OK, water_ok ? "ON" : "OFF", 0, 0, 0);
    esp_mqtt_client_publish(client, T_PUMP, pump_activated ? "ON" : "OFF", 0, 0, 0);

    vTaskDelay(pdMS_TO_TICKS(MQTT_FLUSH_MS));
    ESP_LOGI(TAG, "Published: moisture=%d%% water=%s bat=%dmV pump=%s",
        moisture_pct, water_ok ? "OK" : "empty", battery_mv,
        pump_activated ? "ON" : "OFF");

cleanup:
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    esp_wifi_disconnect();
    esp_wifi_stop();
}

#endif /* USE_HA_MQTT */
