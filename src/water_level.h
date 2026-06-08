#pragma once

#include <stdbool.h>

/* Call once at startup */
void water_level_init(void);

/*
 * Returns true if the tank has water.
 *
 * Reed switch at TOP of float range:
 *   Water present → float UP → magnet near switch → CLOSED → GPIO LOW  → true
 *   Tank empty    → float DOWN → magnet away       → OPEN   → GPIO HIGH → false
 */
bool water_level_ok(void);
