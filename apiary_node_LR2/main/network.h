#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

void network_start(QueueHandle_t measurements);
const char *network_setup_ssid(void);
const char *network_setup_password(void);
