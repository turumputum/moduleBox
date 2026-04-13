// ***************************************************************************
// TITLE
//      Lidars - Common functions
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdio.h>
#include "lidars.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include <stdreport.h>

extern configuration me_config;
extern stateStruct me_state;
extern const uint8_t gamma_8[256];

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "LIDARS";


void lidars_report(lidars_t *lidar, uint8_t slot_num) {
    // Apply EMA filter to distance
    if (lidar->filterK < 1.0f) {
        lidar->filteredDist = lidar->filteredDist * (1.0f - lidar->filterK) + (float)lidar->currentDist * lidar->filterK;
    } else {
        lidar->filteredDist = (float)lidar->currentDist;
    }

    uint16_t reportDist = (uint16_t)lidar->filteredDist;

    // Debounce check
    if (lidar->debounceGap > 0 && (xTaskGetTickCount() - lidar->lastTick < lidar->debounceGap)) {
        return;
    }

    if (lidar->distThreshold > 0) {
        // Threshold (discrete) mode - binary output
        uint8_t newState;
        if (lidar->state == 0) {
            // Currently inactive, activate when distance < threshold
            newState = (reportDist < lidar->distThreshold) ? 1 : 0;
        } else {
            // Currently active, deactivate when distance > threshold + hysteresis
            newState = (reportDist > (uint32_t)(lidar->distThreshold + lidar->thresholdHysteresis)) ? 0 : 1;
        }

        if (newState != lidar->prevState) {
            uint8_t reportState = lidar->thresholdInverse ? !newState : newState;
            stdreport_i(lidar->stateReport, reportState);

            // LED: ON when active, OFF when inactive
            if (lidar->ledc_chan.channel >= 0) {
                ledc_set_duty(LEDC_MODE, lidar->ledc_chan.channel, 254 * reportState);
                ledc_update_duty(LEDC_MODE, lidar->ledc_chan.channel);
            }

            lidar->prevState = newState;
            lidar->lastTick = xTaskGetTickCount();
            ESP_LOGD(TAG, "Threshold state:%d (raw:%d) dist:%d slot:%d", reportState, newState, reportDist, slot_num);
        }
        lidar->state = newState;

    } else if (lidar->flag_distance_only) {
        // Distance-only mode
        if (abs((int)reportDist - (int)lidar->prevDist) > lidar->deadBand) {
            stdreport_i(lidar->distanceReport, reportDist);

            // LED: brightness inversely proportional to distance (closer = brighter)
            if (lidar->ledc_chan.channel >= 0) {
                uint16_t range = lidar->distMaxVal - lidar->distMinVal;
                float f_res = (range > 0) ? (float)(reportDist - lidar->distMinVal) / range : 0;
                if (f_res > 1) f_res = 1;
                if (f_res < 0) f_res = 0;
                uint8_t dutyVal = gamma_8[(int)(255 - 254 * f_res)];
                ledc_set_duty(LEDC_MODE, lidar->ledc_chan.channel, dutyVal);
                ledc_update_duty(LEDC_MODE, lidar->ledc_chan.channel);
            }

            lidar->prevDist = reportDist;
            lidar->lastTick = xTaskGetTickCount();
        }

    } else {
        // Angle + distance mode (default)
        if (abs((int)reportDist - (int)lidar->prevDist) > lidar->deadBand ||
            lidar->currentAngle != lidar->prevAngle) {
            stdreport_i(lidar->distanceReport, reportDist);
            stdreport_i(lidar->angleReport, lidar->currentAngle);

            // LED: brightness inversely proportional to distance (closer = brighter)
            if (lidar->ledc_chan.channel >= 0) {
                uint16_t range = lidar->distMaxVal - lidar->distMinVal;
                float f_res = (range > 0) ? (float)(reportDist - lidar->distMinVal) / range : 0;
                if (f_res > 1) f_res = 1;
                if (f_res < 0) f_res = 0;
                uint8_t dutyVal = gamma_8[(int)(255 - 254 * f_res)];
                ledc_set_duty(LEDC_MODE, lidar->ledc_chan.channel, dutyVal);
                ledc_update_duty(LEDC_MODE, lidar->ledc_chan.channel);
            }

            lidar->prevDist = reportDist;
            lidar->prevAngle = lidar->currentAngle;
            lidar->lastTick = xTaskGetTickCount();
        }
    }
}
