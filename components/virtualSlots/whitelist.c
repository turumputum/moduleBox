// ***************************************************************************
// TITLE: Whitelist Module
//
// PROJECT: moduleBox
// ***************************************************************************

#include "virtualSlots.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "stateConfig.h"
#include <stdreport.h>
#include <stdcommand.h>
#include "reporter.h"
#include "executor.h"
#include "me_slot_config.h"

#include <generated_files/gen_whitelist.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#define MAX_LINE_LENGTH 256
static const char* TAG = "WHITELIST";

extern configuration me_config;
extern stateStruct me_state;

typedef enum{
    WHITELISTCMD_check = 0,
} WHITELISTCMD;

typedef struct __tag_WHITELIST_CONFIG{
    char filename[MAX_LINE_LENGTH];
    int report;
    STDCOMMANDS cmds;
} WHITELIST_CONFIG, * PWHITELIST_CONFIG;

/*
    Программный модуль для размещения связей во внешнем файле
    Виртуальный слот, не взаимодействует с аппаратной частью
    slots: 0-9
*/
void configure_whitelist(PWHITELIST_CONFIG ch, int slot_num){
    
    strcpy(ch->filename, "/sdcard/");
    
    /* Имя файла со списком, по умолчанию whitelist,txt
    */
    if (strstr(me_config.slot_options[slot_num], "filename") != NULL) {
        strcat(ch->filename, get_option_string_val(slot_num, "filename", ""));
    } else {
        strcat(ch->filename, "whitelist.txt");
    }
    
    if (access(ch->filename, F_OK) != 0) {
        char errorString[300];
        sprintf(errorString, "whitelist file: %s, does not exist", ch->filename);
        ESP_LOGE(TAG, "%s", errorString);
        vTaskDelay(200);
        vTaskDelete(NULL);
    }
    ESP_LOGD(TAG, "Set filename :%s for slot:%d", ch->filename, slot_num);

    /* Не стандартный топик для whitelist
    */
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/whitelist_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "topic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/whitelist_0") + 3];
        sprintf(t_str, "%s/whitelist_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }

    stdcommand_init(&ch->cmds, slot_num);
    
    /* Запуск проверки
    */
    stdcommand_register(&ch->cmds, WHITELISTCMD_check, "check", PARAMT_string);

    /* Возвращает если совпадений не найдено
    */
    ch->report = stdreport_register(RPTT_int, slot_num, "", "noMatches", 0, 1);
}

void whitelist_task(void *arg) {
    int slot_num = *(int*) arg;

    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));
    WHITELIST_CONFIG c = {0};
    configure_whitelist(&c, slot_num);
    STDCOMMAND_PARAMS params = {0};

    waitForWorkPermit(slot_num);
    
    while(1){
        int cmd = stdcommand_receive(&c.cmds, &params, portMAX_DELAY);
        char * cmd_arg = (params.count > 0) ? params.p[0].p : (char *)"0";
        ESP_LOGD(TAG, "Slot_num:%d cmd:%d arg:%s", slot_num, cmd, cmd_arg);
        
        switch (cmd){
            case -1: // none
                break;

            case WHITELISTCMD_check: 
                int count = 0;
                if(cmd_arg != NULL){
                    FILE* file = fopen(c.filename, "r");
                    if (file == NULL) {
                        ESP_LOGE(TAG, "Failed to open file");
                        goto end;
                    }
                    char line[MAX_LINE_LENGTH];
                    
                    while (fgets(line, sizeof(line), file)) {
                        line[strcspn(line, "\n")] = '\0';
                        
                        char* validValue = NULL;
                        char* command = NULL;
                        if(strstr(line, "->") != NULL){
                            validValue = strtok_r(line, "->", &command);
                            command++;
                        } else {
                            ESP_LOGW(TAG, "whitelist wrong format");
                            goto end;
                        }
                        
                        if (strcmp(validValue, cmd_arg) == 0) {
                            char output_action[strlen(me_config.deviceName) + strlen(command) + 2];
                            sprintf(output_action, "%s/%s", me_config.deviceName, command);
                            execute(output_action);
                            count++;
                        }
                    }
                    end:
                    fclose(file);
                    
                    if(count == 0){
                        stdreport_i(c.report, 1);
                    }
                }
                break;
        }
    }
}

void start_whitelist_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "whitelist_task_%d", slot_num);
    xTaskCreatePinnedToCore(whitelist_task, tmpString, 1024*4, &t_slot_num, configMAX_PRIORITIES - 20, NULL, 0);
    ESP_LOGD(TAG, "whitelist_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_whitelist()
{
    return manifesto;
}
