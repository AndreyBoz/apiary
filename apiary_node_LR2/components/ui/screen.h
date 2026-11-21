#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    float grams;
    float dbfs;
    int millivolts;
    int charge;
    bool battery_ok;
} measurement_t;

esp_err_t screen_init(void);
esp_err_t screen_draw(const measurement_t *data, bool gateway, bool menu, bool cursor);

esp_err_t screen_draw_setup(const char *ssid, const char *password);
