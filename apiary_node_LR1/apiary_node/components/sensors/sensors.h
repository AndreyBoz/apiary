#pragma once

#include <stdbool.h>
#include <stdint.h>

void sensors_init(void);
bool scale_read(int32_t *raw);
void audio_begin(void);
bool audio_poll(float *rms);
