#pragma once

typedef struct {
    int threshold_pct;      /* water when moisture drops below this % */
    int raw_dry;            /* ADC reading in open air (0% moisture)   */
    int raw_wet;            /* ADC reading fully submerged (100%)      */
    int pump_duration_ms;   /* how long to run the pump each cycle     */
    int pump_cooldown_ms;   /* wait after watering before next check   */
    int check_interval_ms;  /* how often to read sensor when soil OK   */
    int tank_empty_recheck_ms; /* how often to recheck when tank is empty */
} watering_config_t;

/* Single global config instance — read/write directly, then call config_save() */
extern watering_config_t g_config;

/* Load from NVS on startup (falls back to defaults if first boot) */
void config_init(void);

/* Persist g_config to NVS — call after changing any field */
void config_save(void);

/* Print all values to serial */
void config_print(void);
