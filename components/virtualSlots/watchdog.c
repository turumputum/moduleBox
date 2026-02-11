// ***************************************************************************
// TITLE: Watchdog Module
//
// PROJECT: moduleBox
// ***************************************************************************

#include "virtualSlots.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_system.h"
#include "stateConfig.h"
#include <stdreport.h>
#include <stdcommand.h>
#include "reporter.h"
#include "executor.h"
#include "me_slot_config.h"

// #include <generated_files/gen_watchdog.h>
// const char * get_manifest_watchdog()
// {
//     return manifesto;
// }

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "WATCHDOG";

extern configuration me_config;
extern stateStruct me_state;

typedef struct __tag_WATCHDOG_CONFIG{
    uint32_t time;
} WATCHDOG_CONFIG, * PWATCHDOG_CONFIG;

/* 
    Виртуальный модуль watchdog
    Перезагружает устройство по таймауту
    slots: 6-9
*/
void configure_watchdog(PWATCHDOG_CONFIG ch, int slot_num)
{
    /* Время до срабатывания watchdog в секундах
       По умолчанию 3600 с
    */
    ch->time = get_option_int_val(slot_num, "time", "s", 3600, 1, UINT32_MAX);
    ESP_LOGD(TAG, "Set time :%ld for slot:%d", ch->time, slot_num);

    char t_str[strlen(me_config.deviceName) + strlen("/watchdog_0") + 3];
    sprintf(t_str, "%s/watchdog_%d", me_config.deviceName, slot_num);
    me_state.trigger_topic_list[slot_num] = strdup(t_str);
    me_state.action_topic_list[slot_num] = strdup(t_str);
    ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
}

static void IRAM_ATTR watchdog_isr_handler(void* arg){
    int slot_num = (int) arg;
    uint8_t tmp = 1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

void watchdog_task(void *arg) {
    int slot_num = *(int*) arg;

    me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));
    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    WATCHDOG_CONFIG c = {0};
    configure_watchdog(&c, slot_num);

    int strl = strlen(me_state.action_topic_list[slot_num]) + strlen("restart") + 1;
    char tmpstr[strl];
    sprintf(tmpstr, "%s/%s", me_state.action_topic_list[slot_num], "restart");
    xQueueSend(me_state.command_queue[slot_num], &tmpstr, NULL);
    
    esp_timer_handle_t virtual_timer = NULL;
    const esp_timer_create_args_t delay_timer_args = {
        .callback = &watchdog_isr_handler,
        .arg = (void*)slot_num,
        .name = "watchdog_timer"
    };
    esp_timer_create(&delay_timer_args, &virtual_timer);

    waitForWorkPermit(slot_num);

    while(1){
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t tmp;
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, 0) == pdPASS){
            ESP_LOGD(TAG, "%ld :: watchdog reset", xTaskGetTickCount());
            vTaskDelay(1000);
            esp_restart();
        }
        
        command_message_t cmd;
        if (xQueueReceive(me_state.command_queue[slot_num], &cmd, 0) == pdPASS){
            char *command = cmd.str + strlen(me_state.action_topic_list[slot_num]) + 1;
            
            if(!memcmp(command, "restart", 7)){ 
                if(esp_timer_is_active(virtual_timer)){
                    esp_timer_stop(virtual_timer);
                }
                esp_timer_start_once(virtual_timer, c.time * 1000 * 1000);  
                ESP_LOGD(TAG, "Start watchdog on time:%ld", c.time);
            }
        }
    }
}

void start_watchdog_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "watchdog_task_%d", slot_num);
    xTaskCreatePinnedToCore(watchdog_task, tmpString, 1024*4, &t_slot_num, configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "watchdog_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

