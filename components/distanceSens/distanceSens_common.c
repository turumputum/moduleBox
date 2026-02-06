#include <stdio.h>
#include "distanceSens.h"

#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/ledc.h"

#include "reporter.h"
#include "stateConfig.h"

#include "esp_log.h"
#include "me_slot_config.h"

#include <mbdebug.h>
#include <stdreport.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "DISTANCE_SENS";

extern const uint8_t gamma_8[256];

uint16_t crc16_modbus(uint8_t *data, uint8_t length) {
    uint16_t crc = 0xFFFF;
    
    for (uint8_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc = crc >> 1;
            }
        }
    }
    
    return crc;
}

void distanceSens_report(distanceSens_t *distanceSens, uint8_t slot_num) {
    if(distanceSens->inverse){
        distanceSens->currentPos= distanceSens->maxVal - distanceSens->currentPos;
    }
    if (distanceSens->k < 1) {
        distanceSens->currentPos = (float)distanceSens->prevPos * (1 - distanceSens->k) + (float)distanceSens->currentPos * distanceSens->k;
    }
    if(distanceSens->currentPos>distanceSens->maxVal){
        distanceSens->currentPos=distanceSens->maxVal;
    }else if(distanceSens->currentPos<distanceSens->minVal){
        distanceSens->currentPos=distanceSens->minVal;
    }
    
    if(xTaskGetTickCount()- distanceSens->lastTick > distanceSens->debounceGap){
        if (distanceSens->threshold > 0) {
            uint8_t newState = (distanceSens->currentPos > distanceSens->threshold) ? distanceSens->inverse : !distanceSens->inverse;
            
            // Проверяем, находимся ли мы в режиме cooldown
            if (distanceSens->inCooldown) {
                // Если во время cooldown снова переходим из 0 в 1, перезапускаем таймер
                if (newState == 1 && distanceSens->prevState == 0) {
                    distanceSens->cooldownStartTick = xTaskGetTickCount();
                    distanceSens->state = newState;
                    distanceSens->prevPos = distanceSens->currentPos;
                    ESP_LOGD(TAG, "Cooldown restarted for slot:%d (0->1 during cooldown)", slot_num);
                    return;
                }
                
                // Проверяем, истек ли период cooldown
                if (xTaskGetTickCount() - distanceSens->cooldownStartTick >= distanceSens->cooldownTime) {
                    // Cooldown завершен
                    distanceSens->inCooldown = 0;
                    ESP_LOGD(TAG, "Cooldown finished for slot:%d", slot_num);
                } else {
                    // Еще в cooldown - не отправляем рапорты
                    distanceSens->state = newState;
                    distanceSens->prevPos = distanceSens->currentPos;
                    return;
                }
            }
            
            // Не в cooldown - проверяем изменение состояния
            if (newState != distanceSens->prevState) {
                stdreport_i(distanceSens->distanceReport, newState);

                ledc_set_duty(LEDC_MODE, distanceSens->ledc_chan.channel, 254 * newState);
                ledc_update_duty(LEDC_MODE, distanceSens->ledc_chan.channel);

                distanceSens->prevState = newState;
                distanceSens->lastTick = xTaskGetTickCount();
                
                // Если перешли в активное состояние (1) и настроен cooldownTime, запускаем cooldown
                if (newState == 1 && distanceSens->cooldownTime > 0) {
                    distanceSens->inCooldown = 1;
                    distanceSens->cooldownStartTick = xTaskGetTickCount();
                    ESP_LOGD(TAG, "Cooldown started for slot:%d, duration:%ld ms", slot_num, pdTICKS_TO_MS(distanceSens->cooldownTime));
                }
            }
            distanceSens->state = newState;
        }else{
            if(abs(distanceSens->currentPos-distanceSens->prevPos)> distanceSens->deadBand){
                float f_res =  (float)distanceSens->currentPos / (distanceSens->maxVal - distanceSens->minVal);
                if(f_res > 1) f_res = 1;
                if(f_res < 0) f_res = 0;
                
                if (distanceSens->flag_float_output == 1) {     
                    stdreport_f(distanceSens->distanceReport, f_res);
                }else{
                    stdreport_i(distanceSens->distanceReport, distanceSens->currentPos);
                }

                distanceSens->lastTick = xTaskGetTickCount();
                uint8_t dutyVal = gamma_8[(int)(255-254*f_res)];
                ledc_set_duty(LEDC_MODE, distanceSens->ledc_chan.channel, dutyVal);
                ledc_update_duty(LEDC_MODE, distanceSens->ledc_chan.channel);
            }
        }
        distanceSens->prevPos = distanceSens->currentPos;
    }
}
