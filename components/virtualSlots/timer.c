// ***************************************************************************
// TITLE: Timer Module
//
// PROJECT: moduleBox
// ***************************************************************************

#include "virtualSlots.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "stateConfig.h"
#include <stdreport.h>
#include <stdcommand.h>
#include "reporter.h"
#include "me_slot_config.h" 

#include <generated_files/gen_timer.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "TIMER";

extern configuration me_config;
extern stateStruct me_state;

typedef struct __tag_TIMER_CONFIG{
    uint32_t time;
    int timerEndReport;
    STDCOMMANDS cmds;
} TIMER_CONFIG, * PTIMER_CONFIG;

typedef enum{
    TIMERCMD_start = 0,
    TIMERCMD_stop = 1,
} TIMERCMD;

/* 
    Виртуальный модуль таймер
    slots: 6-9
*/
void configure_timer(PTIMER_CONFIG ch, int slot_num){
    
    /* Время через которое сработает таймер. Единицы измерения мс.
       По умолчанию 1000 мс
    */
    ch->time = get_option_int_val(slot_num, "time", "ms", 1000, 1, UINT32_MAX);
    ESP_LOGD(TAG, "Set time :%ld for slot:%d", ch->time, slot_num);

    /* Не стандартный топик для таймера
    */
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/timer_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "customTopic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/timer_0") + 3];
        sprintf(t_str, "%s/timer_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }

    stdcommand_init(&ch->cmds, slot_num);
    
    /* Запустить таймер
       Опционально - время таймера, если параметр не задан или равен 0 будет использован параметр заданный в опциях
    */
    stdcommand_register(&ch->cmds, TIMERCMD_start, "start", PARAMT_int);

    /* Остановить таймер
    */
    stdcommand_register(&ch->cmds, TIMERCMD_stop, "stop", PARAMT_none);

    /* Отчёт о срабатывании таймера
    */
    ch->timerEndReport = stdreport_register(RPTT_string, slot_num, "", "timerEnd");
}

static void IRAM_ATTR timer_isr_handler(void* arg){
    int slot_num = (int) arg;
    uint8_t tmp = 1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

void timer_task(void *arg) {
    int slot_num = *(int*) arg;
    STDCOMMAND_PARAMS params = {0};
    params.skipTypeChecking = true;
    
    me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));
    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    TIMER_CONFIG c = {0};
    configure_timer(&c, slot_num);
    
    esp_timer_handle_t virtual_timer;
    const esp_timer_create_args_t delay_timer_args = {
        .callback = &timer_isr_handler,
        .arg = (void*)slot_num,
        .name = "virtual_timer"
    };
    esp_timer_create(&delay_timer_args, &virtual_timer);

    waitForWorkPermit(slot_num);

    while(1){
        uint8_t tmp;
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, 10) == pdPASS){
            stdreport_s(c.timerEndReport, "");
        }

        int cmd = stdcommand_receive(&c.cmds, &params, 5);
        char * cmd_arg = (params.count > 0) ? params.p[0].p : (char *)"0";
        
        switch (cmd){
            case -1: // none
                break;

            case TIMERCMD_start: 
                int val = atoi(cmd_arg);
                if(val == 0){
                    val = c.time;
                }

                if(esp_timer_is_active(virtual_timer)){
                    esp_timer_stop(virtual_timer);
                }

                esp_timer_start_once(virtual_timer, (val - 5) * 1000);
                break;
                
            case TIMERCMD_stop:
                esp_err_t ret = esp_timer_stop(virtual_timer);
                if(ret != ESP_OK){
                    ESP_LOGE(TAG, "stop timer error:%s", esp_err_to_name(ret));
                }
                break;
        }
    }
}

void start_timer_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "timer_task_%d", slot_num);
    xTaskCreatePinnedToCore(timer_task, tmpString, 1024*4, &t_slot_num, configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "timer_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_timer()
{
    return manifesto;
}
