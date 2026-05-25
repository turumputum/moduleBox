// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "buttonMatrix.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "executor.h"
#include "reporter.h"
#include "stateConfig.h"
#include "me_slot_config.h"
#include "stdreport.h"
#include <mbdebug.h>

#include <generated_files/gen_buttonMatrix.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

static const char *TAG = "BUTTON_MATRIX";
#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

static void parse_slot_list(const char *src, int *slots, int *count, int max) {
    char buf[strlen(src) + 1];
    strcpy(buf, src);
    char *saveptr = NULL;
    char *first = strtok_r(buf, ",", &saveptr);
    if (first == NULL) {
        return;
    }
    char *token = strtok(first, " ");
    while (token != NULL && *count < max) {
        slots[(*count)++] = atoi(token);
        token = strtok(NULL, " ");
    }
}

/*
    Модуль матрицы кнопок
    сканирует прямоугольную матрицу: outSlots задаёт колонки выводов, inSlots - строки чтения
    при нажатии репортит символ из mapping по индексу row*cols + col
*/
void configure_buttonMatrix(buttonMatrix_t *ctx, int slot_num) {
    
    if (strstr(me_config.slot_options[slot_num], "outSlots:") != NULL) {
        /* Список номеров слотов задействованных как выходы матрицы (через пробел)
        по умолчанию используется текущий слот
        */
        char *raw = get_option_string_val(slot_num, "outSlots", "0");
        parse_slot_list(raw, ctx->outSlots, &ctx->outSlotsCount, BUTTON_MATRIX_MAX_SLOTS);
    }
    if (ctx->outSlotsCount == 0) {
        ctx->outSlots[0]   = slot_num;
        ctx->outSlotsCount = 1;
    }
    for (int i = 0; i < ctx->outSlotsCount; i++) {
        ctx->out_pin[i * 3]     = SLOTS_PIN_MAP[ctx->outSlots[i]][0];
        ctx->out_pin[i * 3 + 1] = SLOTS_PIN_MAP[ctx->outSlots[i]][1];
        ctx->out_pin[i * 3 + 2] = SLOTS_PIN_MAP[ctx->outSlots[i]][2];
        ESP_LOGD(TAG, "Slot:%d outSlot[%d]=%d pins:%d %d %d",
                 slot_num, i, ctx->outSlots[i],
                 ctx->out_pin[i * 3], ctx->out_pin[i * 3 + 1], ctx->out_pin[i * 3 + 2]);
    }


    if (strstr(me_config.slot_options[slot_num], "inSlots:") != NULL) {
        /* Список номеров слотов задействованных как входы матрицы (через пробел)
        по умолчанию используется слот следующий за текущим
        */
        char *raw = get_option_string_val(slot_num, "inSlots", "1");
        parse_slot_list(raw, ctx->inSlots, &ctx->inSlotsCount, BUTTON_MATRIX_MAX_SLOTS);
    }
    if (ctx->inSlotsCount == 0) {
        ctx->inSlots[0]   = slot_num + 1;
        ctx->inSlotsCount = 1;
    }
    for (int i = 0; i < ctx->inSlotsCount; i++) {
        ctx->in_pin[i * 3]     = SLOTS_PIN_MAP[ctx->inSlots[i]][0];
        ctx->in_pin[i * 3 + 1] = SLOTS_PIN_MAP[ctx->inSlots[i]][1];
        ctx->in_pin[i * 3 + 2] = SLOTS_PIN_MAP[ctx->inSlots[i]][2];
        ESP_LOGD(TAG, "Slot:%d inSlot[%d]=%d pins:%d %d %d",
                 slot_num, i, ctx->inSlots[i],
                 ctx->in_pin[i * 3], ctx->in_pin[i * 3 + 1], ctx->in_pin[i * 3 + 2]);
    }

    
    ctx->charMapSize = ctx->outSlotsCount * 3 * ctx->inSlotsCount * 3;
    ctx->charMap     = calloc(ctx->charMapSize + 1, sizeof(char));
    if (strstr(me_config.slot_options[slot_num], "mapping:") != NULL) {
        /* Строка соответствий клеток матрицы и символов
        длина строки должна совпадать с outSlotsCount * inSlotsCount * 9,
        без пробелов, например '123456789'
        при отсутствии опции клетки автоматически нумеруются: a-z затем A-Z
        */
        char *raw = get_option_string_val(slot_num, "mapping", "123456789");
        int len = strlen(raw);
        if (len > ctx->charMapSize) {
            len = ctx->charMapSize;
        }
        memcpy(ctx->charMap, raw, len);
    } else {
        for (int i = 0; i < ctx->charMapSize; i++) {
            if (i < 26)      ctx->charMap[i] = 'a' + i;
            else if (i < 52) ctx->charMap[i] = 'A' + (i - 26);
            else             ctx->charMap[i] = '?';
        }
    }
    ESP_LOGD(TAG, "Slot:%d mapping:%s", slot_num, ctx->charMap);

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char *custom_topic = get_option_string_val(slot_num, "topic", "/buttonMatrix_0");
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "buttonMatrix topic:%s", me_state.trigger_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/buttonMatrix_0") + 3];
        sprintf(t_str, "%s/buttonMatrix_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standart buttonMatrix topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    /* Рапортует строкой символ соответствующий нажатой клетке матрицы
       символ берётся из mapping по индексу row*cols + col
    */
    ctx->charReport = stdreport_register(RPTT_string, slot_num, "", "key");
}

void buttonMatrix_task(void *arg) {
    int slot_num = (int)(intptr_t)arg;

    buttonMatrix_t ctx = BUTTON_MATRIX_DEFAULT();
    configure_buttonMatrix(&ctx, slot_num);

    int rows = ctx.outSlotsCount * 3;
    int cols = ctx.inSlotsCount * 3;

    for (int x = 0; x < rows; x++) {
        for (int y = 0; y < cols; y++) {
            if (ctx.out_pin[x] == ctx.in_pin[y]) {
                char errorString[50];
                sprintf(errorString, "buttonMatrix_%d wrong slots config", slot_num);
                ESP_LOGE(TAG, "%s", errorString);
                mblog(E, errorString);
                free(ctx.charMap);
                vTaskDelete(NULL);
            }
        }
    }

    for (int i = 0; i < rows; i++) {
        gpio_config_t io_conf = {};
        io_conf.intr_type    = GPIO_INTR_DISABLE;
        io_conf.mode         = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << ctx.out_pin[i];
        io_conf.pull_down_en = 0;
        io_conf.pull_up_en   = 0;
        if (gpio_config(&io_conf) != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config failed for output pin:%d", ctx.out_pin[i]);
        }
        ESP_LOGD(TAG, "Set output pin: %d", ctx.out_pin[i]);
    }

    for (int i = 0; i < cols; i++) {
        gpio_config_t io_conf = {};
        io_conf.intr_type    = GPIO_INTR_DISABLE;
        io_conf.mode         = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = 1ULL << ctx.in_pin[i];
        io_conf.pull_down_en = 1;
        io_conf.pull_up_en   = 0;
        if (gpio_config(&io_conf) != ESP_OK) {
            ESP_LOGE(TAG, "gpio_config failed for input pin:%d", ctx.in_pin[i]);
        }
        ESP_LOGD(TAG, "Set input pin: %d", ctx.in_pin[i]);
    }

    for (int i = 0; i < rows; i++) {
        gpio_set_level(ctx.out_pin[i], 0);
    }

    int countMatrix[rows][cols];
    memset(countMatrix, 0, sizeof(countMatrix));
    uint8_t resMatrix[rows][cols];
    memset(resMatrix, 0, sizeof(resMatrix));
    uint8_t prevResMatrix[rows][cols];
    memset(prevResMatrix, 0, sizeof(prevResMatrix));
    const uint8_t maxCount = 6;

    waitForWorkPermit(slot_num);

    while (1) {
        vTaskDelay(15 / portTICK_PERIOD_MS);

        for (int r = 0; r < rows; r++) {
            gpio_set_level(ctx.out_pin[r], 1);
            vTaskDelay(1 / portTICK_PERIOD_MS);
            for (int c = 0; c < cols; c++) {
                if (gpio_get_level(ctx.in_pin[c]) == 1) {
                    countMatrix[r][c] += 1;
                    if (countMatrix[r][c] > maxCount) countMatrix[r][c] = maxCount;
                } else {
                    countMatrix[r][c] -= 1;
                    if (countMatrix[r][c] < 0) countMatrix[r][c] = 0;
                }

                if (countMatrix[r][c] > (maxCount / 2) + 1) {
                    resMatrix[r][c] = 1;
                } else if (countMatrix[r][c] < (maxCount / 2) - 1) {
                    resMatrix[r][c] = 0;
                }
            }
            gpio_set_level(ctx.out_pin[r], 0);
        }

        if (memcmp(resMatrix, prevResMatrix, sizeof(resMatrix)) != 0) {
            for (int r = 0; r < rows; r++) {
                for (int c = 0; c < cols; c++) {
                    prevResMatrix[r][c] = resMatrix[r][c];
                    if (resMatrix[r][c] == 1) {
                        ESP_LOGD(TAG, "buttonMatrix slot:%d row:%d col:%d", slot_num, r, c);
                        char str[2] = {0};
                        str[0] = ctx.charMap[r * cols + c];
                        ESP_LOGD(TAG, "report:%s", str);
                        stdreport_s(ctx.charReport, str);
                    }
                }
            }
        }
    }
}

void start_buttonMatrix_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(buttonMatrix_task, "buttonMatrix_task", 1024 * 4, (void*)(intptr_t)slot_num, configMAX_PRIORITIES - 12, NULL);
    ESP_LOGD(TAG, "buttonMatrix_task init ok: %d Heap usage: %lu free heap:%u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char *get_manifest_buttonMatrix() {
    return manifesto;
}
