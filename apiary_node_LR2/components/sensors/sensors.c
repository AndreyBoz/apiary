#include "sensors.h"
#include <math.h>
#include "node_config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define AUDIO_SIZE 1024
#define AUDIO_WINDOWS 4

static portMUX_TYPE hx_lock = portMUX_INITIALIZER_UNLOCKED;
static i2s_chan_handle_t mic;
static int32_t samples[AUDIO_SIZE];
static float hann[AUDIO_SIZE];
static float hann_gain;
static unsigned sample_count;
static unsigned windows_left;
static float rms_total;
static int64_t capture_started;

void sensors_init(void)
{
    gpio_config_t dout = {
        .pin_bit_mask = 1ULL << PIN_HX_DOUT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dout));
    ESP_ERROR_CHECK(gpio_set_direction(PIN_HX_CLOCK, GPIO_MODE_OUTPUT));
    gpio_set_level(PIN_HX_CLOCK, 0);

    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel.dma_desc_num = 4;
    channel.dma_frame_num = 256;
    ESP_ERROR_CHECK(i2s_new_channel(&channel, NULL, &mic));
    i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_BCLK,
            .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_DATA,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(mic, &config));
    ESP_ERROR_CHECK(i2s_channel_enable(mic));

    float energy = 0;
    for (unsigned i = 0; i < AUDIO_SIZE; ++i) {
        hann[i] = 0.5f - 0.5f * cosf(6.28318530718f * i / (AUDIO_SIZE - 1));
        energy += hann[i] * hann[i];
    }
    hann_gain = sqrtf(AUDIO_SIZE / energy);
}

bool scale_read(int32_t *raw)
{
    if (gpio_get_level(PIN_HX_DOUT)) {
        return false;
    }

    uint32_t bits = 0;
    portENTER_CRITICAL(&hx_lock);
    for (unsigned i = 0; i < 25; ++i) {
        gpio_set_level(PIN_HX_CLOCK, 1);
        esp_rom_delay_us(1);
        if (i < 24) {
            bits = (bits << 1) | gpio_get_level(PIN_HX_DOUT);
        }
        gpio_set_level(PIN_HX_CLOCK, 0);
        esp_rom_delay_us(1);
    }
    portEXIT_CRITICAL(&hx_lock);
    *raw = (int32_t)bits - ((bits & 0x800000) ? 0x1000000 : 0);
    return true;
}

void audio_begin(void)
{
    sample_count = 0;
    windows_left = AUDIO_WINDOWS;
    rms_total = 0;
    capture_started = esp_timer_get_time();
}

bool audio_poll(float *rms)
{
    static int32_t frames[256 * 2];
    size_t received = 0;
    i2s_channel_read(mic, frames, sizeof(frames), &received, 1);
    if (!windows_left) {
        return false;
    }
    if (esp_timer_get_time() - capture_started > 1000000) {
        windows_left = 0;
        *rms = NAN;
        return true;
    }
    for (unsigned i = 0; i + 1 < received / sizeof(frames[0]); i += 2) {
        samples[sample_count++] = frames[i] >> 8;
        if (sample_count != AUDIO_SIZE) {
            continue;
        }
        int64_t sum = 0;
        for (unsigned j = 0; j < AUDIO_SIZE; ++j) {
            sum += samples[j];
        }
        const float mean = (float)sum / AUDIO_SIZE;
        float energy = 0;
        for (unsigned j = 0; j < AUDIO_SIZE; ++j) {
            const float adjusted = (samples[j] - mean) * hann[j];
            energy += adjusted * adjusted;
        }
        rms_total += sqrtf(energy / AUDIO_SIZE) * hann_gain;
        sample_count = 0;
        if (--windows_left == 0) {
            *rms = rms_total / AUDIO_WINDOWS;
            return true;
        }
    }
    return false;
}
