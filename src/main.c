/*
 * Auto Watering System
 *
 * Wiring:
 *   Sensor VCC       -> GPIO4 (Left-4)  powered only during measurement
 *   Sensor GND       -> GND   (Left-2)
 *   Sensor AOUT      -> GPIO1 (Left-7)
 *   Reed switch      -> GPIO3 (Left-5) and GND (other leg)
 *   GPIO5 (Right-1)  --[1kΩ]-- KT829A Base
 *   KT829A Collector -> pump (-)
 *   KT829A Emitter   -> GND
 *   5V (Left-1)      -> pump (+)
 *   Flyback diode across pump: stripe toward 5V
 *   GPIO0 (Left-8)   --[220Ω]-- LED anode, LED cathode -> GND  (tank-empty warning)
 */

#include "config.h"
#include "water_level.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MOISTURE_CHANNEL    ADC_CHANNEL_1  /* GPIO1 */
#define SENSOR_POWER_PIN    GPIO_NUM_4     /* sensor VCC — on only during measurement */
#define PUMP_PIN            GPIO_NUM_5
#define WARNING_LED_PIN     GPIO_NUM_0     /* external LED, active HIGH */
#define ADC_SAMPLES         16
#define SENSOR_WARMUP_MS    10            /* time for sensor to stabilise after power-on */
#define TANK_EMPTY_SLEEP_S  30            /* re-check interval while tank is empty */

static const char *TAG = "watering";
static adc_oneshot_unit_handle_t s_adc;

/* ── Warning LED ─────────────────────────────────────────────────────────── */

static void warning_led_init(void)
{
    gpio_hold_dis(WARNING_LED_PIN);    /* release hold set before previous sleep */
    gpio_reset_pin(WARNING_LED_PIN);
    gpio_set_direction(WARNING_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(WARNING_LED_PIN, 0);
}

/* ── Moisture sensor ─────────────────────────────────────────────────────── */

static void adc_init(void)
{
    gpio_hold_dis(SENSOR_POWER_PIN);   /* release hold set before previous sleep */
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
    gpio_hold_dis(PUMP_PIN);           /* release hold set before previous sleep */
    gpio_reset_pin(PUMP_PIN);
    gpio_set_direction(PUMP_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PUMP_PIN, 0);
}

static void pump_set(int on)
{
    gpio_set_level(PUMP_PIN, on);
    ESP_LOGI(TAG, "pump %s", on ? "ON" : "off");
}

/* ── Sleep ───────────────────────────────────────────────────────────────── */

static void go_to_sleep(uint32_t ms)
{
    /*
     * Hold pump and sensor power pins LOW during sleep so the pump can't
     * accidentally activate if the GPIO floats after power-down.
     * WARNING_LED_PIN is held separately by the caller (HIGH when tank empty,
     * not held otherwise so it resets to LOW on wake).
     */
    gpio_hold_en(PUMP_PIN);
    gpio_hold_en(SENSOR_POWER_PIN);

    /*
     * gpio_hold_en() alone is sufficient for RTC-capable pads (GPIO0-5).
     * GPIO6+ are digital-only pads and also need the global deep-sleep hold.
     * WARNING_LED_PIN is GPIO6, so this call is what actually keeps it latched.
     */
    gpio_deep_sleep_hold_en();

    ESP_LOGI(TAG, "Sleeping %lu ms", (unsigned long)ms);

    /* Let the UART finish transmitting before the chip powers down */
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
    esp_deep_sleep_start();
    /* never reached */
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void app_main(void)
{
    /*
     * The ESP32-C3 uses built-in USB-CDC for serial. Deep sleep powers it down,
     * so the host drops the connection. Wait for USB to re-enumerate before
     * logging anything, otherwise all output is lost.
     */
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();

    /* Release the global deep-sleep pad hold that was set before the last sleep */
    gpio_deep_sleep_hold_dis();

    vTaskDelay(pdMS_TO_TICKS(500));

    config_init();

    /* Only print the full config on first boot to avoid spamming the log */
    if (wakeup != ESP_SLEEP_WAKEUP_TIMER) {
        config_print();
    }

    warning_led_init();
    adc_init();
    pump_init();
    water_level_init();

    /* Tank empty — keep LED on during sleep as a continuous warning */
    if (!water_level_ok()) {
        ESP_LOGW(TAG, "Tank empty — rechecking in %d s", TANK_EMPTY_SLEEP_S);
        gpio_set_level(WARNING_LED_PIN, 1);
        gpio_hold_en(WARNING_LED_PIN);
        go_to_sleep(TANK_EMPTY_SLEEP_S * 1000);
    }

    int pct = read_moisture_pct();
    ESP_LOGI(TAG, "moisture: %d%%", pct);

    if (pct < g_config.threshold_pct) {
        ESP_LOGI(TAG, "Dry — watering for %d ms", g_config.pump_duration_ms);
        pump_set(1);
        vTaskDelay(pdMS_TO_TICKS(g_config.pump_duration_ms));
        pump_set(0);
        go_to_sleep(g_config.pump_cooldown_ms);
    } else {
        go_to_sleep(g_config.check_interval_ms);
    }
}
