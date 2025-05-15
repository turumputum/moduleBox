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




//+++++++++++++++++++++++++++++++++stepper++++++++++++++++++++++++++++++++++++++++

#define STATE_ACCEL 0
#define STATE_RUN 1
#define STATE_DECEL 2
#define STATE_STOP 3

void stepper_getCurrentPos(stepper_t *stepper){
    int32_t pos = 0;
    pcnt_unit_get_count(stepper->pcntUnit, &pos);
    
    int32_t delta= pos - stepper->pcnt_prevPos;
    if (abs(delta) > (INT16_MAX/2)) {
        // Произошло переполнение
        if(delta>0){
            //переполнение в отрицательной зоне
            delta = pos -(stepper->pcnt_prevPos - INT16_MIN);
            ESP_LOGD(TAG, "overload in negative zone");
        }else{
            delta = pos - (stepper->pcnt_prevPos - INT16_MAX);
            ESP_LOGD(TAG, "overload in positive zone");
        }
    }
    
    stepper->currentPos = stepper->currentPos + delta;
    //ESP_LOGD(TAG, "currentPos: %ld pos:%ld prevPos:%ld delta:%ld dir:%d", stepper->currentPos, pos, stepper->pcnt_prevPos, delta, stepper->dir);
    stepper->pcnt_prevPos = pos;
}

void stepper_stop(stepper_t *stepper) {
    stepper->targetPos = stepper->currentPos;
    stepper->runSpeedFlag = 0;
    stepper->currentSpeed = 0;
    stepper->targetSpeed = 0;
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(stepper->mcpwmTimer, MCPWM_TIMER_STOP_FULL));
    stepper->state=STOP;
    //ESP_LOGD(TAG, "stopped");
}

void stepper_break(stepper_t *stepper) {
    stepper_getCurrentPos(stepper);
    int64_t chisl = (((int64_t)stepper->currentSpeed * (int64_t)stepper->currentSpeed));
    float znam = 2 * stepper->accel;
    float accel_distance = chisl / znam;
    int64_t target =accel_distance*stepper->dir; 
    stepper_moveTo(stepper, stepper->currentPos+target);
    ESP_LOGD(TAG, "accel_distance: %f stopPoint: %lld", accel_distance, target);
}

static bool IRAM_ATTR pcnt_on_target_reached(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *user_data) {
    stepper_t* stepper = (stepper_t*) user_data;
    stepper_getCurrentPos(stepper);
    //ESP_LOGD(TAG, "currentPos: %ld", stepper->currentPos);
    // pcnt_unit_remove_watch_point(stepper->pcntUnit, stepper->currentPos);
    //stepper->state++;
    if(stepper->currentPos == stepper->targetPos){
        // Останавливаем двигатель
        stepper_stop(stepper);
        pcnt_unit_clear_count(stepper->pcntUnit);
        stepper->pcnt_prevPos = 0;
    }
    int32_t tmpCnt = 0;
    pcnt_unit_get_count(stepper->pcntUnit, &tmpCnt);
    if(tmpCnt!=stepper->pcnt_watchPoint){
        pcnt_unit_clear_count(stepper->pcntUnit);
        stepper->pcnt_prevPos = 0;
    }
    // else if(stepper->currentPos == stepper->breakPoint){
    //     stepper->targetSpeed = 0;
    // }
    //pcnt_unit_remove_watch_point(stepper->pcntUnit, stepper->currentPos);
    return true;
}


void stepper_init(stepper_t *stepper, gpio_num_t step_pin, gpio_num_t dir_pin, uint8_t pulseWidth){
	stepper->stepPin = step_pin;
    stepper->dirPin = dir_pin;

    //-----------------------pcnt init------------------------------
    pcnt_unit_config_t unit_config = {
        .high_limit = INT16_MAX,
        .low_limit = INT16_MIN,
        .flags.accum_count = true, // accumulate the counter value
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &stepper->pcntUnit));

    // PCNT channel configuration
    pcnt_chan_config_t chan_config = {
        .edge_gpio_num = stepper->stepPin,
        .level_gpio_num = stepper->dirPin, // No level GPIO
    };
    ESP_ERROR_CHECK(pcnt_new_channel(stepper->pcntUnit, &chan_config, &stepper->pcntChan));
    
        //Настраиваем счет в зависимости от DIR
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(stepper->pcntChan,
                                                  PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                  PCNT_CHANNEL_EDGE_ACTION_HOLD));
    
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(stepper->pcntChan,
                                                  PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    // Set initial counter value
    //ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(pcnt, &(pcnt_glitch_filter_config_t){.max_glitch_ns = 1000}), TAG, "Set glitch filter failed");
    
    // Register event callbacks
    pcnt_event_callbacks_t cbs = {
        .on_reach = pcnt_on_target_reached,
    };
    ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(stepper->pcntUnit, &cbs, stepper));
    //ESP_ERROR_CHECK(pcnt_unit_add_watch_point(stepper->pcntUnit, INT16_MAX-1));
    //ESP_ERROR_CHECK(pcnt_unit_add_watch_point(stepper->pcntUnit, INT16_MIN+1));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(stepper->pcntUnit, stepper->pcnt_watchPoint));

    ESP_ERROR_CHECK(pcnt_unit_enable(stepper->pcntUnit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(stepper->pcntUnit));
    ESP_ERROR_CHECK(pcnt_unit_start(stepper->pcntUnit));

    //----------------dir config--------------------------------
    gpio_config_t en_dir_gpio_config = {
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = 1ULL << stepper->stepPin | 1ULL << stepper->dirPin,
    };
    ESP_ERROR_CHECK(gpio_config(&en_dir_gpio_config));

    //-----------------------mcpwm init------------------------------
    mcpwm_timer_config_t timer_config = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = stepper->resolution,  // 1MHz
        .period_ticks = 100,      // 1KHz
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .flags.update_period_on_empty = true,
    }; 
    ESP_ERROR_CHECK(mcpwm_new_timer(&timer_config, &stepper->mcpwmTimer));

    mcpwm_operator_config_t operator_config = {
        .group_id = 0,
    };
    ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &stepper->mcpwmOper));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(stepper->mcpwmOper, stepper->mcpwmTimer));

    mcpwm_comparator_config_t comparator_config = {
        .flags.update_cmp_on_tez = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_comparator(stepper->mcpwmOper, &comparator_config, &stepper->mcpwmComparator));
    
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(stepper->mcpwmComparator, pulseWidth));

    mcpwm_generator_config_t generator_config = {
        .gen_gpio_num = stepper->stepPin,
        .flags.io_loop_back = true,
    };
    ESP_ERROR_CHECK(mcpwm_new_generator(stepper->mcpwmOper, &generator_config, &stepper->mcpwmGenerator));

    // go high on counter empty
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(stepper->mcpwmGenerator,MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    // go low on compare threshold
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(stepper->mcpwmGenerator,MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, stepper->mcpwmComparator, MCPWM_GEN_ACTION_LOW)));


    // Запуск таймера
    ESP_ERROR_CHECK(mcpwm_timer_enable(stepper->mcpwmTimer));
    ESP_LOGD(TAG, "init speedStepper end");
}


void stepper_checkDir(stepper_t *stepper){
    if((stepper->dir==DIR_CW)&&(stepper->currentPos > stepper->targetPos)){
        //надо менять направление вращения
        ESP_LOGD(TAG, "Dir change needed, CW");
        stepper->targetSpeed = 0;
    }else if((stepper->dir==DIR_CCW)&&(stepper->currentPos < stepper->targetPos)){
        //надо менять направление вращения
        ESP_LOGD(TAG, "Dir change needed, CCW");
        stepper->targetSpeed = 0;
    }

    if(stepper->currentSpeed==0){
        int8_t dir = stepper->targetPos > stepper->currentPos ? DIR_CW : DIR_CCW;
        if(dir!=stepper->dir){
            stepper->dir=dir;
            gpio_set_level(stepper->dirPin, dir==DIR_CW ? !stepper->dirInverse : stepper->dirInverse);
            ESP_LOGD(TAG, "Dir changed, newDir:%s dirInverse:%d", stepper->dir==DIR_CW?"CW":"CCW", stepper->dirInverse);
            stepper->targetSpeed = stepper->maxSpeed;
        }
        
    }
}

void stepper_moveTo(stepper_t *stepper, int32_t pos){
    stepper_getCurrentPos(stepper);
    stepper->targetPos = pos;
    stepper->targetSpeed = stepper->maxSpeed;
    
    int64_t distance = stepper->targetPos - stepper->currentPos;
    if(distance==0){
        return;
    }

    //!!! убери меня если checkDir заработает
    // stepper->dir = distance > 0 ? DIR_CW : DIR_CCW;
    // gpio_set_level(stepper->dirPin, stepper->dir==DIR_CW ? !stepper->dirInverse : stepper->dirInverse);
    // ESP_LOGD(TAG, "dir:%d dirInverse:%d", stepper->dir, stepper->dirInverse);

    //pcnt_unit_stop(stepper->pcntUnit);
    
    pcnt_unit_remove_watch_point(stepper->pcntUnit, stepper->pcnt_watchPoint);
    pcnt_unit_remove_watch_point(stepper->pcntUnit, INT16_MAX);
    pcnt_unit_remove_watch_point(stepper->pcntUnit, INT16_MIN);
    int32_t watchPoint = distance;

    stepper_checkDir(stepper);

    if(stepper->dir==DIR_CW){
        while(watchPoint>INT16_MAX){
            watchPoint-=INT16_MAX;
        }
    }else if(stepper->dir==DIR_CCW){
        while(watchPoint<INT16_MIN){
            watchPoint+=INT16_MIN;
        }
    }
    stepper->pcnt_watchPoint = watchPoint;

    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(stepper->pcntUnit, stepper->pcnt_watchPoint));
    if(stepper->dir==DIR_CW){
        pcnt_unit_add_watch_point(stepper->pcntUnit, INT16_MAX);
    }else if(stepper->dir==DIR_CCW){
        pcnt_unit_add_watch_point(stepper->pcntUnit, INT16_MIN);
    }
    //ESP_LOGD(TAG, "add watch point %d", stepper->pcnt_watchPoint);

    ESP_ERROR_CHECK(pcnt_unit_clear_count(stepper->pcntUnit));
    stepper->pcnt_prevPos = 0;
    
    
    //pcnt_unit_start(stepper->pcntUnit);
    int64_t chisl = (((int64_t)stepper->maxSpeed * (int64_t)stepper->maxSpeed) - ((int64_t)stepper->currentSpeed * (int64_t)stepper->currentSpeed));
    float znam = 2 * stepper->accel;
    float accel_distance = chisl / znam;
    //ESP_LOGD(TAG, "chisl:%lld znam:%f accel_distance: %f maxSpeed: %ld currentSpeed: %ld", chisl, znam, accel_distance, stepper->maxSpeed, stepper->currentSpeed);
    if(accel_distance>abs(distance/2)){
        accel_distance = abs(distance/2);
    }
    int8_t dir;
    if(stepper->currentPos > stepper->targetPos){
        dir=1;
    }else if(stepper->currentPos < stepper->targetPos){
        dir=-1;
    }
    stepper->breakPoint = stepper->targetPos-(accel_distance * stepper->dir);
    
    if(stepper->state==STOP){
        mcpwm_timer_set_period(stepper->mcpwmTimer, UINT16_MAX);
        ESP_ERROR_CHECK(mcpwm_timer_start_stop(stepper->mcpwmTimer, MCPWM_TIMER_START_NO_STOP));
        stepper->state=RUN;
    }

    ESP_LOGD(TAG, "currentPos:%ld targetPos:%ld watchPoint:%d accel_distance: %f breakPoint: %ld  state:%s", stepper->currentPos, stepper->targetPos, stepper->pcnt_watchPoint, accel_distance, stepper->breakPoint, stepper->state==STOP?"STOP":"RUN");
    ESP_LOGD(TAG, "currentSpeed: %ld targetSpeed: %ld dir:%s", stepper->currentSpeed, stepper->targetSpeed, stepper->dir==DIR_CW?"CW":"CCW");
}

void stepper_speedUpdate(stepper_t *stepper, int32_t period){  
    if(stepper->runSpeedFlag==1){
        stepper->currentPos=0;
    }

    if(stepper->currentPos!=stepper->targetPos){
        if((stepper->dir==DIR_CW)&&(stepper->currentPos>=stepper->breakPoint)){
            //если достигли точки торможения при вращении по часовой стрелке    
            stepper->targetSpeed=0;
            //ESP_LOGD(TAG,"Lets breaking, curPos:%ld breakPoint:%ld dir:%s", stepper->currentPos, stepper->breakPoint, stepper->dir==DIR_CW?"CW":"CCW");
        }else if((stepper->dir==DIR_CCW)&&(stepper->currentPos<=stepper->breakPoint)){
            //если достигли точки торможения при вращении против часовой стрелке    
            stepper->targetSpeed=0;
            //ESP_LOGD(TAG,"Lets breaking, curPos:%ld breakPoint:%ld dir:%s", stepper->currentPos, stepper->breakPoint, stepper->dir==DIR_CW?"CW":"CCW");

        }
        

        stepper_checkDir(stepper);

        //ESP_LOGD(TAG, "currentSpeed: %ld targetSpeed: %ld period: %ld", stepper->currentSpeed, stepper->targetSpeed, period);
    
        if(stepper->targetSpeed!=stepper->currentSpeed){
            int32_t speedIncrement = (stepper->accel/1000)*period;
            if (speedIncrement<1)speedIncrement=1;
            
            if(stepper->currentSpeed<stepper->targetSpeed){
                stepper->currentSpeed += speedIncrement;
                if(stepper->currentSpeed>stepper->targetSpeed){
                    stepper->currentSpeed = stepper->targetSpeed;
                }
            }else{
                stepper->currentSpeed -= speedIncrement;
                if(stepper->currentSpeed<stepper->targetSpeed){
                    stepper->currentSpeed = stepper->targetSpeed;
                }
            }
            if(abs(stepper->currentSpeed)>0){
                uint32_t period = abs((int32_t)stepper->resolution/stepper->currentSpeed);
                if(period>UINT16_MAX)period=UINT16_MAX;
                //ESP_LOGD(TAG, "currentSpeed: %ld targetSpeed: %ld speedIncrement: %ld period: %ld currentPos:%ld targetPos:%ld", stepper->currentSpeed, stepper->targetSpeed, speedIncrement, period, stepper->currentPos, stepper->targetPos);
                mcpwm_timer_set_period(stepper->mcpwmTimer, period);
            }else if((stepper->currentPos==stepper->targetPos)&&(stepper->state!=STOP)){
                stepper_stop(stepper);
            }
        }

    }
        
}



void stepper_setZero(stepper_t *stepper) {
    ESP_ERROR_CHECK(pcnt_unit_clear_count(stepper->pcntUnit));
    stepper->pcnt_prevPos = 0;
    stepper->currentPos = 0;
}
