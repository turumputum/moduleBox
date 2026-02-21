// ***************************************************************************
// TITLE: Phone Dialer Module
//
// PROJECT: moduleBox
// ***************************************************************************

#include "dialer.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdcommand.h>
#include <stdreport.h>
#include "reporter.h"
#include "stateConfig.h"
#include "me_slot_config.h"

#include <generated_files/gen_dialer.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "PHONE_DIALER";

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

typedef enum {
    DIALERCMD_reset = 0,
} DIALERCMD;

static void IRAM_ATTR gpio_isr_handler(void* arg){
    int slot_num = (int) arg;
    uint8_t tmp = 1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

/*
    Модуль дискового телефонного номеронабирателя
    Считывает импульсы с дискового набирателя и собирает номер
*/
void configure_dialer(PDIALER_CONFIG ch, int slot_num)
{
    /* Время ожидания окончания набора в миллисекундах
       По умолчанию 3000 мс
    */
    ch->waitingTime = get_option_int_val(slot_num, "waitingTime", "ms", 3000, 1, 60000);
    ESP_LOGD(TAG, "Set waitingTime:%d for slot:%d", ch->waitingTime, slot_num);

    /* Максимальная длина набираемого номера
       По умолчанию 7 символов
    */
    ch->numberMaxLenght = get_option_int_val(slot_num, "numberMaxLenght", "", 7, 1, 32);
    ESP_LOGD(TAG, "Set numberMaxLenght:%d for slot:%d", ch->numberMaxLenght, slot_num);

    /* Инверсия сигнала разрешения набора
       0-1 по умолчанию 0
    */
    ch->enaInverse = get_option_flag_val(slot_num, "enaInverse");
    ESP_LOGD(TAG, "Set enaInverse:%d for slot:%d", ch->enaInverse, slot_num);

    /* Инверсия импульсного сигнала
       0-1 по умолчанию 0
    */
    ch->pulseInverse = get_option_flag_val(slot_num, "pulseInverse");
    ESP_LOGD(TAG, "Set pulseInverse:%d for slot:%d", ch->pulseInverse, slot_num);

    /* Время антидребезга импульсного сигнала в миллисекундах
       По умолчанию 20 мс
    */
    ch->debounceGap = get_option_int_val(slot_num, "debounceGap", "ms", 20, 1, 1000);
    ESP_LOGD(TAG, "Set debounceGap:%d for slot:%d", ch->debounceGap, slot_num);

    /* Не стандартный топик для номеронабирателя
    */
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic", "/dialer_0");
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "topic:%s", me_state.trigger_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/dialer_0") + 3];
        sprintf(t_str, "%s/dialer_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    stdcommand_init(&ch->cmds, slot_num);

    /* Сброс текущего набираемого номера
    */
    stdcommand_register(&ch->cmds, DIALERCMD_reset, "reset", PARAMT_none);

    /* Отчёт набранного номера
    */
    ch->numberReport = stdreport_register(RPTT_string, slot_num, "", "");
}

void dialer_task(void* arg) {
    int slot_num = *(int*)arg;
    uint8_t ena_pin = SLOTS_PIN_MAP[slot_num][1];
    uint8_t pulse_pin = SLOTS_PIN_MAP[slot_num][0];

    me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));
    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    DIALER_CONFIG c = {0};
    configure_dialer(&c, slot_num);
    STDCOMMAND_PARAMS params = {0};

    // Настройка GPIO разрешения набора
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << ena_pin),
        .pull_down_en = 1,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);

    // Настройка GPIO импульсного входа
    gpio_reset_pin(pulse_pin);
    esp_rom_gpio_pad_select_gpio(pulse_pin);
    gpio_config_t in_conf = {
        .intr_type = c.pulseInverse ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << pulse_pin),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&in_conf);
    gpio_isr_handler_add(pulse_pin, gpio_isr_handler, (void*)slot_num);

    uint8_t counter = 0;
    char number_str[c.numberMaxLenght + 1];
    memset(number_str, 0, sizeof(number_str));
    uint16_t string_lenght = 0;
    uint8_t prev_ena_state = 0;
    uint32_t dial_start_time = 0;
    uint8_t state_flag = 0;

    waitForWorkPermit(slot_num);

    while(1){
        // Обработка команд
        int cmd = stdcommand_receive(&c.cmds, &params, 0);
        switch (cmd){
            case -1:
                break;
            case DIALERCMD_reset:
                memset(number_str, 0, sizeof(number_str));
                string_lenght = 0;
                state_flag = 0;
                counter = 0;
                ESP_LOGD(TAG, "Reset dialer for slot:%d", slot_num);
                break;
        }

        // Обработка сигнала разрешения набора
        uint8_t ena_state = gpio_get_level(ena_pin) ? c.enaInverse : !c.enaInverse;
        if(ena_state != prev_ena_state){
            prev_ena_state = ena_state;
            dial_start_time = pdTICKS_TO_MS(xTaskGetTickCount());
            if((ena_state == !c.enaInverse) && (state_flag == 0)){
                state_flag = 1;
                counter = 0;
                ESP_LOGD(TAG, "Lets input number: %s strlen:%d dial_start_time:%ld", number_str, strlen(number_str), dial_start_time);
            } else if(ena_state == c.enaInverse){
                if(counter >= 10) counter = 0;
                number_str[string_lenght] = (char)counter + 48;
                string_lenght++;
                number_str[string_lenght] = '\0';
                ESP_LOGD(TAG, "update number_str: %s counter:%d strlen:%d", number_str, counter, strlen(number_str));
                counter = 0;
            }
        }

        // Проверка завершения набора
        if(state_flag == 1){
            if(((pdTICKS_TO_MS(xTaskGetTickCount()) - dial_start_time) >= c.waitingTime) || (string_lenght >= c.numberMaxLenght)){
                if(ena_state == c.enaInverse){
                    ESP_LOGD(TAG, "Input end, report number: %s", number_str);
                    stdreport_s(c.numberReport, number_str);
                    memset(number_str, 0, c.numberMaxLenght);
                    state_flag = 0;
                    string_lenght = 0;
                    vTaskDelay(50);
                }
            }
        }

        // Обработка импульсов
        uint8_t tmp;
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, pdMS_TO_TICKS(15)) == pdPASS){
            if(gpio_get_level(pulse_pin) == c.pulseInverse){
                counter++;
                vTaskDelay(c.debounceGap);
            }
        }
    }
}

void start_dialer_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "dialer_task_%d", slot_num);
    xTaskCreate(dialer_task, tmpString, 1024 * 4, &t_slot_num, configMAX_PRIORITIES - 18, NULL);
    ESP_LOGD(TAG, "dialer_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_dialer()
{
    return manifesto;
}