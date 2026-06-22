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
    
    if (distanceSens->threshold > 0) {
        // threshold-режим: антидребезг как в кнопках - новое состояние 0/1
        // должно непрерывно продержаться debounceGap прежде чем будет принято
        uint8_t raw = (distanceSens->currentPos > distanceSens->threshold) ? distanceSens->inverse : !distanceSens->inverse;
        TickType_t now = xTaskGetTickCount();

        if (raw != distanceSens->cand_state) {
            distanceSens->cand_state = raw;
            distanceSens->cand_since = now;
        }
        uint8_t stable = (now - distanceSens->cand_since) >= distanceSens->debounceGap;

        // cooldown: пока активен - рапорты подавляем, но устойчивый 0->1 перезапускает таймер
        if (distanceSens->inCooldown) {
            if (stable && distanceSens->cand_state == 1 && distanceSens->prevState == 0) {
                distanceSens->cooldownStartTick = now;
                distanceSens->prevState = 1;
                distanceSens->prevPos = distanceSens->currentPos;
                ESP_LOGD(TAG, "Cooldown restarted for slot:%d (0->1 during cooldown)", slot_num);
                return;
            }
            if (now - distanceSens->cooldownStartTick >= distanceSens->cooldownTime) {
                distanceSens->inCooldown = 0;
                ESP_LOGD(TAG, "Cooldown finished for slot:%d", slot_num);
            } else {
                distanceSens->prevPos = distanceSens->currentPos;
                return;
            }
        }

        // Принимаем состояние только когда оно устойчиво держится debounceGap
        if (stable && distanceSens->cand_state != distanceSens->prevState) {
            uint8_t newState = distanceSens->cand_state;
            stdreport_i(distanceSens->stateReport, newState);

            ledc_set_duty(LEDC_MODE, distanceSens->ledc_chan.channel, 254 * newState);
            ledc_update_duty(LEDC_MODE, distanceSens->ledc_chan.channel);

            distanceSens->prevState = newState;

            // Если перешли в активное состояние (1) и настроен cooldownTime, запускаем cooldown
            if (newState == 1 && distanceSens->cooldownTime > 0) {
                distanceSens->inCooldown = 1;
                distanceSens->cooldownStartTick = now;
                ESP_LOGD(TAG, "Cooldown started for slot:%d, duration:%ld ms", slot_num, pdTICKS_TO_MS(distanceSens->cooldownTime));
            }
        }
        distanceSens->state = distanceSens->cand_state;
    } else {
        // аналоговый режим: только deadBand, без троттлинга
        if (abs(distanceSens->currentPos - distanceSens->prevPos) > distanceSens->deadBand) {
            float f_res = (float)distanceSens->currentPos / (distanceSens->maxVal - distanceSens->minVal);
            if (f_res > 1) f_res = 1;
            if (f_res < 0) f_res = 0;

            if (distanceSens->flag_float_output == 1) {
                stdreport_f(distanceSens->distanceFloatReport, f_res);
            } else {
                stdreport_i(distanceSens->distanceReport, distanceSens->currentPos);
            }

            uint8_t dutyVal = gamma_8[(int)(255 - 254 * f_res)];
            ledc_set_duty(LEDC_MODE, distanceSens->ledc_chan.channel, dutyVal);
            ledc_update_duty(LEDC_MODE, distanceSens->ledc_chan.channel);
            distanceSens->prevPos = distanceSens->currentPos;
        }
    }
}
