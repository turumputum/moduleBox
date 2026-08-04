// ***************************************************************************
// TITLE: List Of Actions Module
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
#include "esp_log.h"
#include "stateConfig.h"
#include <stdreport.h>
#include <stdcommand.h>
#include "reporter.h"
#include "executor.h"
#include "me_slot_config.h"

#include <generated_files/gen_listOfActions.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "LISTOFACTIONS";

extern configuration me_config;
extern stateStruct me_state;

// Максимальная длина строки сценария. Строка длиннее пропускается целиком:
// исполнить её обрезок хуже, чем не исполнить ничего.
#define LOA_LINE_MAX        256
#define LOA_FILENAME_MAX    64
// Глубина очереди имён файлов, ждущих своей очереди на проигрывание.
#define LOA_PENDING_MAX     8

typedef enum{
    LOACMD_execute = 0,
} LOACMD;

typedef struct __tag_LOA_CONFIG{
    int             errFileNotFoundReport;
    int             active_state;
    STDCOMMANDS     cmds;

    /* FIFO имён файлов. Пока проигрывается один сценарий, пришедшие execute
       копятся здесь и отрабатывают следом, один за другим. */
    char            pending             [ LOA_PENDING_MAX ][ LOA_FILENAME_MAX ];
    int             pendingCount;
} LOA_CONFIG, * PLOA_CONFIG;

/*
    Список действий - на одно событие вешается целый файл команд
    Строка файла это правая часть кросслинка без имени устройства
    Виртуальный слот, не взаимодействует с аппаратной частью
    slots: 0-9
*/
void configure_listOfActions(PLOA_CONFIG c, int slot_num){

    /* Если флаг поднят - модуль стартует в выключенном состоянии,
       до прихода action/enable 1 (Конституция §6).
    */
    c->active_state = !get_option_flag_val(slot_num, "disableOnStart");
    ESP_LOGD(TAG, "[listOfActions_%d] Initial active_state:%d", slot_num, c->active_state);

    // Standard topic
    {
        char t_str[strlen(me_config.deviceName) + strlen("/listOfActions_0") + 3];
        sprintf(t_str, "%s/listOfActions_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.action_topic_list[slot_num]);
    }

    stdcommand_init(&c->cmds, slot_num);

    /* Проиграть файл сценария - имя файла в корне SD, например lights,txt
    */
    stdcommand_register(&c->cmds, LOACMD_execute, "action/execute", PARAMT_string);

    /* Имя файла, который не удалось открыть
    */
    c->errFileNotFoundReport = stdreport_register(RPTT_string, slot_num, "", "event/errFileNotFound");

    /* === COMMANDS === */

    /* Включить 1 или выключить 0 модуль
    */
    stdcommand_register(&c->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* Состояние модуля - активен 1 или спит 0
    */
    stdreport_register(RPTT_int, slot_num, "", "event/enable");
}

/* Положить имя файла в хвост очереди. Возвращает 0, если очередь переполнена. */
static int loa_pending_push(PLOA_CONFIG c, const char * name){
    if(c->pendingCount >= LOA_PENDING_MAX) return 0;
    strncpy(c->pending[c->pendingCount], name, LOA_FILENAME_MAX-1);
    c->pending[c->pendingCount][LOA_FILENAME_MAX-1] = '\0';
    c->pendingCount++;
    return 1;
}

/* Снять имя файла с головы очереди. Возвращает 0, если очередь пуста.
   out должен вмещать LOA_FILENAME_MAX байт. */
static int loa_pending_pop(PLOA_CONFIG c, char * out){
    if(c->pendingCount <= 0) return 0;
    strcpy(out, c->pending[0]);
    for(int i=1; i<c->pendingCount; i++){
        strcpy(c->pending[i-1], c->pending[i]);
    }
    c->pendingCount--;
    return 1;
}

/* Разбор одной команды. Вызывается и в холостом ожидании, и между строками
   проигрываемого файла - поэтому единая точка. Возвращает 1, если проигрывание
   надо прервать (модуль выключили). */
static int loa_handle_command(PLOA_CONFIG c, int cmd, PSTDCOMMAND_PARAMS params, int slot_num){
    switch(cmd){
        case -1: // none
            break;

        case STDCMD_ENABLE:
            if(params->count > 0){
                int new_state = params->p[0].i ? 1 : 0;
                if(new_state != c->active_state){
                    c->active_state = new_state;
                    ESP_LOGD(TAG, "[listOfActions_%d] enable:%d", slot_num, c->active_state);
                    stdreport_enable(slot_num, c->active_state);
                    if(!c->active_state){
                        /* Спящий модуль не должен доигрывать начатое и не должен
                           проснуться с накопленным хвостом (Конституция §6). */
                        c->pendingCount = 0;
                        return 1;
                    }
                }
            }
            break;

        case LOACMD_execute:
            if(!c->active_state) break;
            if(params->count > 0 && params->p[0].p != NULL && strlen(params->p[0].p) > 0){
                if(loa_pending_push(c, params->p[0].p)){
                    ESP_LOGD(TAG, "[listOfActions_%d] queued:%s (pending:%d)",
                             slot_num, params->p[0].p, c->pendingCount);
                }else{
                    ESP_LOGW(TAG, "[listOfActions_%d] pending queue full, dropping:%s",
                             slot_num, params->p[0].p);
                }
            }else{
                ESP_LOGW(TAG, "[listOfActions_%d] execute without filename", slot_num);
            }
            break;
    }
    return 0;
}

/* Забрать всё, что успело накопиться в очереди команд, не блокируясь.
   Возвращает 1, если проигрывание надо прервать. */
static int loa_pump_commands(PLOA_CONFIG c, PSTDCOMMAND_PARAMS params, int slot_num){
    int cmd;
    while((cmd = stdcommand_receive(&c->cmds, params, 0)) != -1){
        if(loa_handle_command(c, cmd, params, slot_num)) return 1;
    }
    return 0;
}

/* Проиграть один файл сценария. Каждая непустая строка - правая часть
   кросслинка без имени устройства; дописываем deviceName и отдаём в executor.
   Темпа не задаём: execute() шлёт в executor_queue блокирующе, поэтому на
   переполнении очереди мы просто ждём, пока executor разгребёт, и продолжаем. */
static void loa_play_file(PLOA_CONFIG c, const char * name, PSTDCOMMAND_PARAMS params, int slot_num){
    char path[sizeof("/sdcard/") + LOA_FILENAME_MAX];
    snprintf(path, sizeof(path), "/sdcard/%s", name);

    FILE * file = fopen(path, "r");
    if(file == NULL){
        ESP_LOGW(TAG, "[listOfActions_%d] file not found:%s", slot_num, path);
        stdreport_s(c->errFileNotFoundReport, (char*)name);
        return;
    }
    ESP_LOGD(TAG, "[listOfActions_%d] playing:%s", slot_num, path);

    char line[LOA_LINE_MAX];
    int lineNum = 0;

    while(fgets(line, sizeof(line), file)){
        lineNum++;

        /* Строка не влезла в буфер: fgets разорвал бы её пополам, и хвост ушёл
           бы в executor как отдельная (бессмысленная) команда. Дочитываем
           физическую строку до конца и пропускаем целиком. */
        if(strchr(line, '\n') == NULL && !feof(file)){
            int ch;
            while((ch = fgetc(file)) != EOF && ch != '\n');
            ESP_LOGW(TAG, "[listOfActions_%d] %s:%d line too long, skipped", slot_num, name, lineNum);
            continue;
        }

        line[strcspn(line, "\r\n")] = '\0';

        char * action = line;
        while(*action == ' ' || *action == '\t') action++;

        // пустая строка или комментарий
        if(*action == '\0' || *action == '#' || *action == ';') continue;

        // хвостовые пробелы: иначе топик не совпал бы с action_topic_list
        size_t len = strlen(action);
        while(len > 0 && (action[len-1] == ' ' || action[len-1] == '\t')){
            action[--len] = '\0';
        }

        /* Собираем в буфер ровно на MAX_STRING_LENGTH: снаружи execute() сам
           обрезает длинную строку, но делает это по границе своего буфера -
           надёжнее не доводить до этого и обрезать здесь, с предупреждением. */
        char output_action[MAX_STRING_LENGTH];
        int need = snprintf(output_action, sizeof(output_action), "%s/%s", me_config.deviceName, action);
        if(need >= (int)sizeof(output_action)){
            ESP_LOGW(TAG, "[listOfActions_%d] %s:%d action truncated", slot_num, name, lineNum);
        }

        execute(output_action);

        /* Между строками разбираем накопившиеся команды: так enable 0 обрывает
           длинный сценарий, а новый execute встаёт в очередь, а не теряется. */
        if(loa_pump_commands(c, params, slot_num)){
            ESP_LOGD(TAG, "[listOfActions_%d] playback aborted at %s:%d", slot_num, name, lineNum);
            break;
        }
    }

    fclose(file);
}

void listOfActions_task(void *arg){
    int slot_num = (int)(intptr_t)arg;

    PLOA_CONFIG c = calloc(1, sizeof(LOA_CONFIG));
    STDCOMMAND_PARAMS params = {0};

    me_state.command_queue[slot_num] = xQueueCreate(10, sizeof(command_message_t));
    configure_listOfActions(c, slot_num);

    char filename[LOA_FILENAME_MAX];

    waitForWorkPermit(slot_num);
    stdreport_enable(slot_num, c->active_state);

    while(1){
        int cmd = stdcommand_receive(&c->cmds, &params, portMAX_DELAY);
        loa_handle_command(c, cmd, &params, slot_num);

        /* Файлы проигрываются строго по одному, в порядке поступления команд. */
        while(loa_pending_pop(c, filename)){
            if(!c->active_state){
                c->pendingCount = 0;
                break;
            }
            loa_play_file(c, filename, &params, slot_num);
        }
    }
}

void start_listOfActions_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "task_listOfActions_%d", slot_num);
    xTaskCreatePinnedToCore(listOfActions_task, tmpString, 1024*4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES - 20, NULL, 0);
    ESP_LOGD(TAG, "listOfActions_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_listOfActions()
{
    return manifesto;
}
