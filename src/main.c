/*
 * Auto Watering System
 *
 * Wiring:
 *   Sensor VCC       -> 5V    (Left-1)
 *   Sensor GND       -> GND   (Left-2)
 *   Sensor AOUT      -> GPIO1 (Left-7)
 *   Reed switch      -> GPIO3 (Left-5) and GND (other leg)
 *   GPIO5 (Right-1)  --[1kΩ]-- KT829A Base
 *   KT829A Collector -> pump (-)
 *   KT829A Emitter   -> GND
 *   5V (Left-1)      -> pump (+)
 *   Flyback diode across pump: stripe toward 5V
 *   GPIO4 (Left-4)   -> Sensor VCC  (powered only during measurement)
 *   GPIO6 (Right-2)  --[220Ω]-- LED anode, LED cathode -> GND  (tank-empty warning)
 */

#include "config.h"
#include "water_level.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MOISTURE_CHANNEL      ADC_CHANNEL_1   /* GPIO1 */
#define SENSOR_POWER_PIN      GPIO_NUM_4      /* sensor VCC — on only during measurement */
#define PUMP_PIN              GPIO_NUM_5
#define WARNING_LED_PIN       GPIO_NUM_6      /* external LED, active HIGH */
#define ADC_SAMPLES           16
#define SENSOR_WARMUP_MS      10             /* time for sensor to stabilise after power-on */

#define WARNING_FLASH_MS      500             /* LED on duration per flash  */
#define WARNING_INTERVAL_MS   6000            /* period between flashes     */

static const char *TAG = "watering";
static adc_oneshot_unit_handle_t s_adc;

/* ── Warning LED ─────────────────────────────────────────────────────────── */

static void warning_led_init(void)
{
    gpio_reset_pin(WARNING_LED_PIN);
    gpio_set_direction(WARNING_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(WARNING_LED_PIN, 0);   /* off */
}

/*
 * Block until the tank is refilled, flashing the warning LED once every
 * WARNING_INTERVAL_MS. Returns as soon as water_level_ok() is true.
 */
static void wait_for_water(void)
{
    ESP_LOGW(TAG, "Tank empty — waiting for refill");

    while (!water_level_ok()) {
        gpio_set_level(WARNING_LED_PIN, 1);
        vTaskDelay(WARNING_FLASH_MS / portTICK_PERIOD_MS);
        gpio_set_level(WARNING_LED_PIN, 0);
        vTaskDelay((WARNING_INTERVAL_MS - WARNING_FLASH_MS) / portTICK_PERIOD_MS);
    }

    ESP_LOGI(TAG, "Tank refilled — resuming");
}

/* ── Moisture sensor ─────────────────────────────────────────────────────── */

static void adc_init(void)
{
    gpio_reset_pin(SENSOR_POWER_PIN);
    gpio_set_direction(SENSOR_POWER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(SENSOR_POWER_PIN, 0);

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &s_adc));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, MOISTURE_CHANNEL, &chan_cfg));
}

static int read_moisture_pct(void)
{
    gpio_set_level(SENSOR_POWER_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(SENSOR_WARMUP_MS));

    int32_t sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw;
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc, MOISTURE_CHANNEL, &raw));
        sum += raw;
    }

    gpio_set_level(SENSOR_POWER_PIN, 0);

    int avg = (int)(sum / ADC_SAMPLES);
    ESP_LOGI(TAG, "moisture raw: %d", avg);
    int pct = (g_config.raw_dry - avg) * 100 / (g_config.raw_dry - g_config.raw_wet);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* ── Pump ────────────────────────────────────────────────────────────────── */

static void pump_init(void)
{
    gpio_reset_pin(PUMP_PIN);
    gpio_set_direction(PUMP_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PUMP_PIN, 0);
}

static void pump_set(int on)
{
    gpio_set_level(PUMP_PIN, on);
    ESP_LOGI(TAG, "pump %s", on ? "ON" : "off");
}

/* ── Main loop ───────────────────────────────────────────────────────────── */

void app_main(void)
{
    config_init();
    config_print();

    warning_led_init();
    adc_init();
    pump_init();
    water_level_init();

    ESP_LOGI(TAG, "Ready");

    while (1) {
        if (!water_level_ok()) {
            wait_for_water();
            continue;
        }

        int pct = read_moisture_pct();
        ESP_LOGI(TAG, "moisture: %d%%", pct);

        if (pct < g_config.threshold_pct) {
            ESP_LOGI(TAG, "Dry — watering for %d ms", g_config.pump_duration_ms);
            pump_set(1);
            vTaskDelay(g_config.pump_duration_ms / portTICK_PERIOD_MS);
            pump_set(0);
            vTaskDelay(g_config.pump_cooldown_ms / portTICK_PERIOD_MS);
        } else {
            vTaskDelay(g_config.check_interval_ms / portTICK_PERIOD_MS);
        }
    }
}
