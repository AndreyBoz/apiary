#include "board.h"
#include "node_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static i2c_master_dev_handle_t oled;
static SemaphoreHandle_t i2c_lock;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration;

void board_init(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << PIN_VEXT) | (1ULL << PIN_OLED_RESET) | (1ULL << PIN_ADC_CTRL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));
    gpio_set_level(PIN_VEXT, VEXT_ON_LEVEL);
    gpio_set_level(PIN_ADC_CTRL, 0);
    gpio_set_level(PIN_OLED_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(PIN_OLED_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(30));

    gpio_config_t button = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));
    i2c_device_config_t display_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x3c,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &display_config, &oled));
    i2c_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(i2c_lock ? ESP_OK : ESP_ERR_NO_MEM);

    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adc));
    adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, ADC_CHANNEL_0, &channel));
    adc_cali_curve_fitting_config_t curve = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_0,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&curve, &calibration) != ESP_OK) {
        calibration = NULL;
    }
}

bool board_button_pressed(void)
{
    return gpio_get_level(PIN_BUTTON) == 0;
}

esp_err_t board_oled_write(const unsigned char *data, unsigned length)
{
    if (xSemaphoreTake(i2c_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = i2c_master_transmit(oled, data, length, 60);
    xSemaphoreGive(i2c_lock);
    return result;
}

bool board_battery(int *millivolts, int *percent)
{
    static const int voltage[] = {3300, 3500, 3650, 3750, 3850, 3950, 4050, 4200};
    static const int charge[] = {0, 5, 15, 30, 55, 78, 90, 100};
    if (!calibration) {
        return false;
    }
    gpio_set_level(PIN_ADC_CTRL, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    int sum = 0;
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        int raw, mv;
        if (adc_oneshot_read(adc, ADC_CHANNEL_0, &raw) == ESP_OK &&
            adc_cali_raw_to_voltage(calibration, raw, &mv) == ESP_OK) {
            sum += mv;
            ++count;
        }
    }
    gpio_set_level(PIN_ADC_CTRL, 0);
    if (!count) {
        return false;
    }
    *millivolts = (sum * 49 + count * 5) / (count * 10);
    if (*millivolts < 2500 || *millivolts > 4500) {
        return false;
    }
    *percent = *millivolts >= 4200 ? 100 : 0;
    for (unsigned i = 1; i < sizeof(voltage) / sizeof(voltage[0]); ++i) {
        if (*millivolts >= voltage[i - 1] && *millivolts < voltage[i]) {
            *percent = charge[i - 1] + (*millivolts - voltage[i - 1]) *
                (charge[i] - charge[i - 1]) / (voltage[i] - voltage[i - 1]);
            break;
        }
    }
    return true;
}
