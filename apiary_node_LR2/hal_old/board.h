#pragma once

#include <stdbool.h>
#include "esp_err.h"

void board_init(void);
bool board_button_pressed(void);
bool board_battery(int *millivolts, int *percent);
esp_err_t board_oled_write(const unsigned char *data, unsigned length);
