#include <stdio.h>
#include "servoDev.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include "math.h"
//#include "driver/ledc.h"

#include "reporter.h"
#include "executor.h"
#include "stateConfig.h"

#include "driver/twai.h"

#include "cybergear.h"
#include "cybergear_utils.h"

#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "servoDev";

#define POLLING_RATE_TICKS pdMS_TO_TICKS(20)
#define MASTER_CAN_ID 0
#define MOTOR_CAN_ID 127

#define TWAI_ALERTS ( TWAI_ALERT_RX_DATA | \
    TWAI_ALERT_TX_IDLE | TWAI_ALERT_TX_SUCCESS | \
    TWAI_ALERT_TX_FAILED | TWAI_ALERT_ERR_PASS | \
    TWAI_ALERT_BUS_ERROR )

void servoRod_task(void *arg) {
    int slot_num = *(int*) arg;
    uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    uint16_t refreshPeriod = 20;
    if (strstr(me_config.slot_options[slot_num], "refreshPeriod") != NULL) {
		refreshPeriod = (get_option_int_val(slot_num, "refreshPeriod", "", 10, 1, 4096));
		ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",refreshPeriod, slot_num);
	}

    float kP=10.0f;
	if (strstr(me_config.slot_options[slot_num], "kP")!=NULL){
		kP = get_option_float_val(slot_num, "kP", 10.0f);
	}

    float kI=0.0005f;
	if (strstr(me_config.slot_options[slot_num], "kI")!=NULL){
		kI = get_option_float_val(slot_num, "kI", 0.0005f);
	}

    float fGain=0.0005f;
	if (strstr(me_config.slot_options[slot_num], "fGain")!=NULL){
		fGain = get_option_float_val(slot_num, "fGain", 0.0005f);
	}

    float maxCurrent=1.0f;
	if (strstr(me_config.slot_options[slot_num], "maxCurrent")!=NULL){
		maxCurrent = get_option_float_val(slot_num, "maxCurrent", 1.0f);
	}

    int16_t maxVal = INT16_MAX;
	if (strstr(me_config.slot_options[slot_num], "maxVal")!=NULL){
		maxVal = get_option_int_val(slot_num, "maxVal", "", 10, 1, 4096);
	}

    int16_t minVal = INT16_MIN;
    if (strstr(me_config.slot_options[slot_num], "minVal")!=NULL){
        minVal = get_option_int_val(slot_num, "minVal", "", 10, 1, 4096);
    }

    int16_t targetVal = 0;
    if (strstr(me_config.slot_options[slot_num], "targetVal")!=NULL){
        targetVal = get_option_int_val(slot_num, "targetVal", "", 10, 1, 4096);
    }

    int16_t deadBand = 2;
    if (strstr(me_config.slot_options[slot_num], "deadBand")!=NULL){
        deadBand = get_option_int_val(slot_num, "deadBand", "", 10, 1, 4096);
    }

    uint8_t posReport=0;
	if (strstr(me_config.slot_options[slot_num], "posReport")!=NULL){
		posReport = 1;
	}


    /* initialize configuration structures using macro initializers */
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t) tx_pin, 
        (gpio_num_t) rx_pin, 
        TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_1MBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    /* install TWAI driver */
    ESP_ERROR_CHECK(twai_driver_install(&g_config, &t_config, &f_config));
    ESP_ERROR_CHECK(twai_start());
    ESP_ERROR_CHECK(twai_reconfigure_alerts(TWAI_ALERTS, NULL));

    vTaskDelay(pdMS_TO_TICKS(1000));
        /* initialize cybergear motor */
	cybergear_motor_t cybergear_motor;
	cybergear_init(&cybergear_motor, MASTER_CAN_ID, MOTOR_CAN_ID, POLLING_RATE_TICKS);
	ESP_ERROR_CHECK(cybergear_stop(&cybergear_motor));
	cybergear_set_mode(&cybergear_motor, CYBERGEAR_MODE_CURRENT);
    cybergear_set_current_filter_gain(&cybergear_motor, fGain);
    cybergear_set_current_ki(&cybergear_motor, kI);
    cybergear_set_current_kp(&cybergear_motor, kP);
	cybergear_set_limit_speed(&cybergear_motor, 0.2f);
	cybergear_set_limit_current(&cybergear_motor, maxCurrent);
	cybergear_enable(&cybergear_motor);
    cybergear_set_mech_position_to_zero(&cybergear_motor);


    int16_t pos = 0;
    int16_t prev_pos = 0;

    uint32_t alerts_triggered;
	twai_status_info_t twai_status;
	twai_message_t message;
	cybergear_status_t status;
    TickType_t lastWakeTime = xTaskGetTickCount();

    waitForWorkPermit(slot_num);
    
    while(1){

        /* request status */

        cybergear_request_status(&cybergear_motor);

		/* handle CAN alerts */ 
		twai_read_alerts(&alerts_triggered, POLLING_RATE_TICKS);
		twai_get_status_info(&twai_status);
        while (twai_receive(&message, 0) == ESP_OK){
            cybergear_process_message(&cybergear_motor, &message);
        }
        /* get cybergear status*/
        cybergear_get_status(&cybergear_motor, &status);

        pos = (cybergear_motor.status.position/(4*M_PI))*INT16_MAX;
        if(pos>maxVal) pos = maxVal;
        if(pos<minVal) pos = minVal;

        if (abs(pos - prev_pos) > deadBand){
            prev_pos = pos;
            float ratio=0;
            if(pos>0){
                ratio = -(float)pos/maxVal;
            }else{
                ratio = (float)pos/minVal;
            }
            float setCur = ratio*maxCurrent;
            ESP_LOGD(TAG, "ratio:%f setCur: %f", ratio, setCur);
            cybergear_set_current(&cybergear_motor, setCur);
            // if(posReport){
            //     char tmpString[60];
            //     sprintf(tmpString, "/pos:%d", pos);
            //     report(tmpString, slot_num);
            //     ESP_LOGD(TAG, "%s", tmpString);
            // }
        }

        
        vTaskDelayUntil(&lastWakeTime, refreshPeriod);
    }

}

void start_servoRod_task(uint8_t slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "servoRod_%d", slot_num);
	xTaskCreatePinnedToCore(servoRod_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 12, NULL, 1);
    ESP_LOGD(TAG, "servoRod_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}