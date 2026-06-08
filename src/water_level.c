#include "water_level.h"
#include "driver/gpio.h"

#define WATER_LEVEL_PIN  GPIO_NUM_3

void water_level_init(void)
{
    gpio_reset_pin(WATER_LEVEL_PIN);
    gpio_set_direction(WATER_LEVEL_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(WATER_LEVEL_PIN, GPIO_PULLUP_ONLY);
}

bool water_level_ok(void)
{
    return gpio_get_level(WATER_LEVEL_PIN) == 0;
}
