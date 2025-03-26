#include <stdlib.h>
#include "driver/rmt_tx.h"
#include "driver/pulse_cnt.h"
#include "driver/rmt_encoder.h"
#include "driver/gpio.h"

#define SCURVE_STEPPER_DEFAULT() {\
    .stepPin = 0,\
    .dirPin = 0,\
    .pcntUnit = NULL,\
    .pcntChan = NULL,\
    .rmtHandle = NULL,\
    .accel_encoder = NULL,\
    .uniform_encoder = NULL,\
    .decel_encoder = NULL,\
    .accel = 100,\
    .maxSpeed = 100,\
    .minSpeed = 10,\
    .currentPos = 0,\
    .targetPos = 0,\
    .direction = 0,\
}

// .accel_config = NULL,
// .uniform_config = NULL,
// .decel_config = NULL,

typedef struct {
    gpio_num_t stepPin;
    gpio_num_t dirPin;
    bool dirInverse;

    pcnt_unit_handle_t pcntUnit;
    pcnt_channel_handle_t pcntChan;

    rmt_channel_handle_t rmtHandle;

    rmt_encoder_handle_t accel_encoder;
    uint32_t accel_steps;
    rmt_encoder_handle_t uniform_encoder;
    uint32_t uniform_steps;
    rmt_encoder_handle_t decel_encoder;
    uint32_t decel_steps;

    rmt_transmit_config_t accel_config;
    rmt_transmit_config_t uniform_config;
    rmt_transmit_config_t decel_config;

    uint16_t accel;
    uint32_t maxSpeed;
    uint32_t minSpeed;

    int32_t currentPos;
    int32_t targetPos;

    uint8_t state;
    bool direction;
}sCurveStepper_t;

esp_err_t sCurveStepper_setZero(sCurveStepper_t *stepper);
esp_err_t sCurveStepper_waitMoveDone(sCurveStepper_t *stepper);
esp_err_t sCurveStepper_init(sCurveStepper_t *stepper);
esp_err_t sCurveStepper_moveTo(sCurveStepper_t *stepper, int32_t position);
esp_err_t sCurveStepper_stop(sCurveStepper_t *stepper);