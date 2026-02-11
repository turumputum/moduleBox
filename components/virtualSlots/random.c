// ***************************************************************************
// TITLE: Random Module
//
// PROJECT: moduleBox
// ***************************************************************************

#include "virtualSlots.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "stateConfig.h"
#include "reporter.h"
#include <stdcommand.h>
#include <stdreport.h>
#include "me_slot_config.h"
 
#include <generated_files/gen_random.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "RANDOM";

extern configuration me_config;
extern stateStruct me_state;

typedef enum{
    RNDCMD_gen = 0,
} RNDCMD;

typedef struct __tag_RND_CONFIG{
    int32_t minVal;
    int32_t maxVal;
    int report;
    STDCOMMANDS cmds;
} RND_CONFIG, * PRND_CONFIG;

/*
    Модуль для генерации случайных чисел
    slots: 6-9
*/
void configure_random(PRND_CONFIG ch, int slot_num){
    
    /* Максимальное значение.
       По умолчанию INT32_MAX
    */
    ch->maxVal = get_option_int_val(slot_num, "maxVal", "", INT32_MAX, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set maxVal :%ld for slot:%d", ch->maxVal, slot_num);

    /* Минимальное значение.
       По умолчанию 0
    */
    ch->minVal = get_option_int_val(slot_num, "minVal", "", 0, INT32_MIN, INT32_MAX);
    ESP_LOGD(TAG, "Set minVal :%ld for slot:%d", ch->minVal, slot_num);

    /* Не стандартный топик для генератора
    */
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/random_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "customTopic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/random_0") + 3];
        sprintf(t_str, "%s/random_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }

    stdcommand_init(&ch->cmds, slot_num);
    
    /* Запуск генератора случайных чисел
    */
    stdcommand_register(&ch->cmds, RNDCMD_gen, "generate", PARAMT_none);

    /* Возвращает сгенерированное значение
    */
    ch->report = stdreport_register(RPTT_int, slot_num, "", "", (int)ch->minVal, (int)ch->maxVal);
}

void random_task(void *arg) {
    int slot_num = *(int*) arg;

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));
    RND_CONFIG c = {0};
    configure_random(&c, slot_num);
    STDCOMMAND_PARAMS params = {0};

    waitForWorkPermit(slot_num);

    while(1){
        int cmd = stdcommand_receive(&c.cmds, &params, portMAX_DELAY);
        
        switch (cmd){
            case -1: // none
                break;

            case RNDCMD_gen: 
                int16_t val = (rand() % (c.maxVal - c.minVal + 1)) + c.minVal;
                ESP_LOGD(TAG, "Gen:%d max:%ld min:%ld", val, c.maxVal, c.minVal);
                
                char tmpString[60];
                sprintf(tmpString, "%d", val);
                report(tmpString, slot_num);
                break;
        }
    }
}

void start_random_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "random_task_%d", slot_num);
    xTaskCreatePinnedToCore(random_task, tmpString, 1024*4, &t_slot_num, configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "random_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_random()
{
    return manifesto;
}
