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
 *   GPIO6 (Right-3)  --[220Ω]-- LED anode, LED cathode -> GND  (tank-empty warning)
 *
 * Battery voltage divider (GPIO0 = ADC1_CH0):
 *   Battery(+) --[100kΩ]-- GPIO0 --[47kΩ]-- GND
 *   Vout = Vbat * 47/147  (scales 4.0–5.8V → 1.28–1.85V, well within ADC_ATTEN_DB_12 range)
 */

#include "config.h"
#include "water_level.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define USE_DEEP_SLEEP      0              /* 1 = deep sleep (production), 0 = vTaskDelay (debug) */

#define MOISTURE_CHANNEL    ADC_CHANNEL_1  /* GPIO1 */
#define BATTERY_CHANNEL     ADC_CHANNEL_0  /* GPIO0 — battery voltage divider */
#define SENSOR_POWER_PIN    GPIO_NUM_4     /* sensor VCC — on only during measurement */
#define PUMP_PIN            GPIO_NUM_5
#define WARNING_LED_PIN     GPIO_NUM_6     /* external LED, active HIGH */
#define BATTERY_LED_PIN     GPIO_NUM_7     /* red LED, active HIGH — low-battery indication */
#define ADC_SAMPLES         16

/*
 * Voltage divider: R1=100kΩ, R2=47kΩ → Vout = Vbat * 47/147
 * ADC_ATTEN_DB_6 full-scale ≈ 2200 mV (12-bit = 4095)
 * Vbat_mv = raw * 2200 / 4095 * 147 / 47
 */
#define BATTERY_DIV_NUM     147            /* R1+R2 */
#define BATTERY_DIV_DEN     47             /* R2 */
#define BATTERY_ADC_MV      3100           /* full-scale for ADC_ATTEN_DB_12 */
#define BATTERY_LOW_MV      4400           /* warn below 1.1 V per NiMH cell */
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

static void battery_led_init(void)
{
    gpio_reset_pin(BATTERY_LED_PIN);
    gpio_set_direction(BATTERY_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(BATTERY_LED_PIN, 0);
}

static void boot_indication(void)
{
    for (int ms = 800; ms > 0; ms -= 100) {
        gpio_set_level(BATTERY_LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(BATTERY_LED_PIN, 0);

        gpio_set_level(WARNING_LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(ms));
        gpio_set_level(WARNING_LED_PIN, 0);
    }
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

    adc_oneshot_chan_cfg_t bat_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BATTERY_CHANNEL, &bat_cfg));
}

static int read_battery_mv(void)
{
    int32_t sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int raw;
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc, BATTERY_CHANNEL, &raw));
        sum += raw;
    }
    int avg = (int)(sum / ADC_SAMPLES);
    int vout_mv = avg * BATTERY_ADC_MV / 4095;
    int vbat_mv = vout_mv * BATTERY_DIV_NUM / BATTERY_DIV_DEN;
    ESP_LOGI(TAG, "battery: %d mV (raw %d)", vbat_mv, avg);
    return vbat_mv;
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
    ESP_LOGI(TAG, "Sleeping %lu ms", (unsigned long)ms);

#if USE_DEEP_SLEEP
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

    /* Let the UART finish transmitting before the chip powers down */
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
    esp_deep_sleep_start();
    /* never reached */
#else
    vTaskDelay(pdMS_TO_TICKS(ms));
#endif
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void app_main(void)
{
    bool is_timer_wakeup = false;

#if USE_DEEP_SLEEP
    /*
     * The ESP32-C3 uses built-in USB-CDC for serial. Deep sleep powers it down,
     * so the host drops the connection. Wait for USB to re-enumerate before
     * logging anything, otherwise all output is lost.
     */
    is_timer_wakeup = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);

    /* Release the global deep-sleep pad hold that was set before the last sleep */
    gpio_deep_sleep_hold_dis();
#endif

    vTaskDelay(pdMS_TO_TICKS(500));

    config_init();

    /* Only print the full config on first boot (or every boot in debug mode) */
    if (!is_timer_wakeup) {
        config_print();
    }

    warning_led_init();
    battery_led_init();

    if (!is_timer_wakeup) {
        boot_indication();
    }

    adc_init();
    pump_init();
    water_level_init();

    while (1) {
        int vbat_mv = read_battery_mv();
        gpio_set_level(BATTERY_LED_PIN, vbat_mv < BATTERY_LOW_MV ? 1 : 0);
        if (vbat_mv < BATTERY_LOW_MV) {
            ESP_LOGW(TAG, "Battery low: %d mV", vbat_mv);
        }

        /* Tank empty — keep LED on during sleep as a continuous warning */
        if (!water_level_ok()) {
            ESP_LOGW(TAG, "Tank empty — rechecking in %d s", TANK_EMPTY_SLEEP_S);
            gpio_set_level(WARNING_LED_PIN, 1);
#if USE_DEEP_SLEEP
            gpio_hold_en(WARNING_LED_PIN);
#endif
            go_to_sleep(TANK_EMPTY_SLEEP_S * 1000);
            gpio_set_level(WARNING_LED_PIN, 0);  /* only reached when USE_DEEP_SLEEP=0 */
            continue;
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
}
