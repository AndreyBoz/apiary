#include <inttypes.h>
#include <math.h>
#include "board.h"
#include "network.h"
#include "node_config.h"
#include "screen.h"
#include "sensors.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static QueueHandle_t measurements;
static QueueHandle_t buttons;
static const char *tag = "lab2_app";

static void sensor_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    TickType_t last = xTaskGetTickCount();
    measurement_t data = {.grams = NAN, .dbfs = NAN};
    int64_t start = esp_timer_get_time();
    int64_t next_mass = start + 5000000;
    int64_t next_audio = start;
    int64_t total = 0;
    int count = 0;
    int64_t hx_time = 0;
    int64_t first_read = 0;

    for (;;) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        int64_t now = esp_timer_get_time();
        int32_t raw;
        if (scale_read(&raw)) {
            hx_time = esp_timer_get_time() - now;
            total += raw;
            ++count;
            if (!first_read) {
                first_read = now;
                ESP_LOGI(tag, "HX711 first sample after %" PRId64 " ms", (now - start) / 1000);
            }
        }
        if (now >= next_audio) {
            audio_begin();
            next_audio += 60000000;
        }
        float rms;
        if (audio_poll(&rms)) {
            data.dbfs = isfinite(rms) ? 20.0f * log10f(fmaxf(rms, 1.0f) / 8388608.0f) : NAN;
            ESP_LOGI(tag, "audio 4x1024 rms=%.1f, %.1f dBFS",
                     (double)rms, (double)data.dbfs);
            xQueueOverwrite(measurements, &data);
        }
        if (now >= next_mass) {
            double mean = count ? (double)total / count : NAN;
            data.grams = (float)((mean - SCALE_TARE) / SCALE_COUNTS_PER_GRAM);
            data.battery_ok = board_battery(&data.millivolts, &data.charge);
            xQueueOverwrite(measurements, &data);
            ESP_LOGI(tag, "mass=%.1f g n=%d raw_mean=%.1f hx_read=%" PRId64 " us",
                     (double)data.grams, count, mean, hx_time);
            if (data.battery_ok) {
                ESP_LOGI(tag, "battery=%d mV charge=%d%%", data.millivolts, data.charge);
            }
            total = 0;
            count = 0;
            next_mass += 5000000;
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(2));
    }
}

static void button_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    TickType_t last = xTaskGetTickCount();
    TickType_t changed = last;
    TickType_t pressed = last;
    bool stable = false;
    bool previous = false;

    for (;;) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        TickType_t now = xTaskGetTickCount();
        bool reading = board_button_pressed();
        if (reading != previous) {
            previous = reading;
            changed = now;
        }
        if (reading != stable && now - changed >= pdMS_TO_TICKS(30)) {
            stable = reading;
            if (stable) {
                pressed = changed;
            } else {
                bool long_press = changed - pressed >= pdMS_TO_TICKS(1000);
                if (xQueueSend(buttons, &long_press, 0) != pdTRUE) {
                    ESP_LOGW(tag, "button queue full");
                }
            }
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
    }
}

static void ui_task(void *arg)
{
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));
    TickType_t last = xTaskGetTickCount();
    measurement_t data = {.grams = NAN, .dbfs = NAN};
    bool gateway = false;
    bool menu = false;
    bool cursor = false;

    for (;;) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        xQueuePeek(measurements, &data, 0);
        bool long_press;
        while (xQueueReceive(buttons, &long_press, 0) == pdTRUE) {
            if (!menu && long_press) {
                menu = true;
                cursor = gateway;
                if (WATCHDOG_DEMO) {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
            } else if (menu && long_press) {
                gateway = cursor;
                menu = false;
                ESP_LOGI(tag, "role confirmed: %s", gateway ? "GATEWAY" : "LEAF");
            } else if (menu) {
                cursor = !cursor;
            }
        }
        esp_err_t display = network_setup_ssid()[0]
            ? screen_draw_setup(network_setup_ssid(), network_setup_password())
            : screen_draw(&data, gateway, menu, cursor);
        if (display != ESP_OK) {
            ESP_LOGW(tag, "OLED transfer failed");
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    const char *reset_name = "other";
    if (reason == ESP_RST_TASK_WDT) {
        reset_name = "TASK_WDT";
    } else if (reason == ESP_RST_PANIC) {
        reset_name = "PANIC";
    } else if (reason == ESP_RST_POWERON) {
        reset_name = "POWERON";
    }
    ESP_LOGI(tag, "reset=%s (%d)", reset_name, (int)reason);
    const esp_task_wdt_config_t watchdog = {
        .timeout_ms = 3000,
        .idle_core_mask = (1U << portNUM_PROCESSORS) - 1,
        .trigger_panic = true,
    };
    esp_err_t result = esp_task_wdt_init(&watchdog);
    if (result == ESP_ERR_INVALID_STATE) {
        result = esp_task_wdt_reconfigure(&watchdog);
    }
    ESP_ERROR_CHECK(result);
    board_init();
    result = screen_init();
    if (result != ESP_OK) {
        ESP_LOGW(tag, "OLED init failed: %s", esp_err_to_name(result));
    }
    sensors_init();
    measurements = xQueueCreate(1, sizeof(measurement_t));
    buttons = xQueueCreate(8, sizeof(bool));
    ESP_ERROR_CHECK(measurements && buttons ? ESP_OK : ESP_ERR_NO_MEM);
    network_start(measurements);
    ESP_ERROR_CHECK(xTaskCreate(sensor_task, "sensor", 6144, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(ui_task, "ui", 4096, NULL, 4, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(button_task, "button", 2048, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
