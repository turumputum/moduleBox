#ifndef PPM_GENERATOR_H
#define PPM_GENERATOR_H

#include "driver/gptimer.h"
#include "driver/gpio.h"

typedef struct {
    gptimer_handle_t timer;
    uint8_t pin;
    uint16_t current_value;
    bool pulse_state;
} ppm_generator_t;

// esp_err_t ppm_generator_init(ppm_generator_t *ppm, uint8_t gpio_pin);
// void ppm_generator_set_value(ppm_generator_t *ppm, uint16_t value);
// void ppm_generator_stop(ppm_generator_t *ppm);

void start_ppm_generator_task(uint8_t slot_num);

#endif