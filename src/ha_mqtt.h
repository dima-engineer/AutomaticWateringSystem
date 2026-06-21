#pragma once

#include <stdbool.h>

/*
 * Home Assistant integration via MQTT discovery.
 *
 * Enable by setting USE_HA_MQTT=1 in platformio.ini build_flags and
 * providing WIFI_SSID, WIFI_PASSWORD, MQTT_BROKER_URL.
 *
 * Call ha_mqtt_init() once at startup, then ha_mqtt_publish() each cycle
 * before going to sleep. Both are no-ops when USE_HA_MQTT=0.
 */

#if defined(USE_HA_MQTT) && USE_HA_MQTT

void ha_mqtt_init(void);
void ha_mqtt_publish(int moisture_pct, bool water_ok, int battery_mv, bool pump_activated);

#else

static inline void ha_mqtt_init(void) {}
static inline void ha_mqtt_publish(int m, bool w, int b, bool p)
{
    (void)m; (void)w; (void)b; (void)p;
}

#endif /* USE_HA_MQTT */
