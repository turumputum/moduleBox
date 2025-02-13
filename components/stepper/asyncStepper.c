#include "asyncStepper.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "me_slot_config.h"
#include "driver/gpio.h"
#include "reporter.h"
#include "stateConfig.h"
#include "driver/pulse_cnt.h"
#include "driver/mcpwm_prelude.h"
#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEPPER";

#define DIR_CW 1
#define DIR_CCW -1

// static IRAM_ATTR bool pcnt_on_reach(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *arg) {
//     asyncStepper_t* stepper = (asyncStepper_t*) arg;
//     stepper->_motorStatus = STEPPER_MOTOR_STOPPED;
//     stepper->_currentPosition = stepper->_targetPosition;
//     // Останавливаем двигатель
//     mcpwm_timer_start_stop(stepper->_timer, MCPWM_TIMER_STOP_EMPTY);
//     return false;
// }

void speedStepper_init(speedStepper_t *stepper, gpio_num_t step_pin, gpio_num_t dir_pin, uint8_t pulseWidth){
	stepper->_stepPin = step_pin;
    stepper->_directionPin = dir_pin;

    //-----------------------pcnt init------------------------------
    // pcnt_unit_config_t unit_config = {
    //     .high_limit = INT16_MAX,
    //     .low_limit = INT16_MIN,
    //     .flags.accum_count = true, // accumulate the counter value
    // };
    // ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &stepper->_pcnt_unit));

    // pcnt_chan_config_t chan_config = {
    //     .edge_gpio_num = stepper->_stepPin,  // Use MCPWM step pin output
    //     .level_gpio_num = -1,  // Direction pin
    //     //.flags.io_loop_back = true // Enable input/output loop back mode
    // };
    // ESP_ERROR_CHECK(pcnt_new_channel(stepper->_pcnt_unit, &chan_config, &stepper->_pcnt_chan));

    // //Настраиваем счет в зависимости от DIR
    // ESP_ERROR_CHECK(pcnt_channel_set_edge_action(stepper->_pcnt_chan,
    //                                               PCNT_CHANNEL_EDGE_ACTION_INCREASE,
    //                                               PCNT_CHANNEL_EDGE_ACTION_HOLD));
    
    // ESP_ERROR_CHECK(pcnt_channel_set_level_action(stepper->_pcnt_chan,
    //                                               PCNT_CHANNEL_LEVEL_ACTION_KEEP,
    //                                               PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ///-----------------------mcpwm init------------------------------
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = stepper->_resolution,  // 1MHz
        .period_ticks = 100,      // 1KHz
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .flags.update_period_on_empty = true,
    }; 
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &stepper->_timer));

    mcpwm_operator_config_t operator_config = {
        .group_id = 0,
    };
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &stepper->_oper));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(stepper->_oper, stepper->_timer));

    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(stepper->_oper, &comparator_config, &stepper->_comparator));
    
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(stepper->_comparator, pulseWidth));

    mcpwm_generator_config_t generator_config = {
        .gen_gpio_num = stepper->_stepPin,
        .flags.io_loop_back = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_generator(stepper->_oper, &generator_config, &stepper->_generator));

    // go high on counter empty
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(stepper->_generator,
                                                              MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    // go low on compare threshold
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(stepper->_generator,
                                                                MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, stepper->_comparator, MCPWM_GEN_ACTION_LOW)));


    // Настройка GPIO для DIR
    gpio_config_t dir_gpio = {
        .pin_bit_mask = (1ULL << stepper->_directionPin),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&dir_gpio));


    // Запуск таймера
    ESP_ERROR_CHECK(mcpwm_timer_enable(stepper->_timer));
    ESP_LOGD(TAG, "init speedStepper end");
}


// void set_stop_position(asyncStepper_t *stepper, int32_t target){
//     ESP_ERROR_CHECK(pcnt_unit_add_watch_point(stepper->_pcnt_unit, target));
// }

// int32_t get_position(asyncStepper_t *stepper) {
//     int value;
//     pcnt_unit_get_count(stepper->_pcnt_unit, &value);
//     //ESP_LOGD(TAG, "position: %d", value);
//     return value;
// }

// void reset_position(asyncStepper_t *stepper) {
//     pcnt_unit_clear_count(stepper->_pcnt_unit);
// }

void speedStepper_stop(speedStepper_t *stepper) {
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(stepper->_timer, MCPWM_TIMER_STOP_FULL));
    stepper->_state=SPEED_STEPPER_STOP;
}

void speedStepper_start(speedStepper_t *stepper) {
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(stepper->_timer, MCPWM_TIMER_START_NO_STOP));
    stepper->_state=SPEED_STEPPER_RUN;
}

void speedStepper_setSpeed(speedStepper_t *stepper, int32_t spd) {
    if(spd==0){
        return;
    }
    uint32_t period = stepper->_resolution/abs(spd);
    if(period < stepper->_minPeriod){
        period = stepper->_minPeriod;
    }
    mcpwm_timer_set_period(stepper->_timer, period);
    //ESP_LOGD(TAG, "setPer %ld", period);
}

void speedStepper_setDirection(speedStepper_t *stepper, int8_t clockwise) {
    //ESP_LOGD(TAG, "setDirection %d", clockwise);
    if(clockwise==1){
        gpio_set_level(stepper->_directionPin, !stepper->_dirInverse);
    }else{
        gpio_set_level(stepper->_directionPin, stepper->_dirInverse);
    }
}

