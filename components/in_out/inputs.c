// ***************************************************************************
// TITLE: Multi-Channel Input Modules (in_2ch, in_3ch)
//
// PROJECT: moduleBox
// ***************************************************************************

#include "in_out.h"
#include "sdkconfig.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_timer.h"
#include "executor.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "stdreport.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

// ОБЯЗАТЕЛЬНО: Включение сгенерированных манифестов
#include <generated_files/gen_inputs.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "IN_OUT";


// =============================================================================
// DATA STRUCTURES
// =============================================================================

// --- in_2ch context ---
typedef struct {
    int slot_num;
    int pin_0;
    int pin_1;
    int inverse_0;
    int inverse_1;
    int stat_0;
    int stat_1;
    int prevState_0;
    int prevState_1;
    int debounceGap;
    int logic;                      // INDEPENDENT_MODE, OR_LOGIC_MODE, AND_LOGIC_MODE
    int stateReport_0;
    int stateReport_1;
    int stateReport_combined;       // For OR/AND logic
    TickType_t lastTick;
    int refreshPeriod;
} in_2ch_context_t;

#define IN_2CH_CONTEXT_DEFAULT() { \
    .slot_num = 0, \
    .pin_0 = 0, \
    .pin_1 = 0, \
    .inverse_0 = 0, \
    .inverse_1 = 0, \
    .stat_0 = 0, \
    .stat_1 = 0, \
    .prevState_0 = 0, \
    .prevState_1 = 0, \
    .debounceGap = 10, \
    .logic = INDEPENDENT_MODE, \
    .stateReport_0 = 0, \
    .stateReport_1 = 0, \
    .stateReport_combined = 0, \
    .lastTick = 0, \
    .refreshPeriod = 100 \
}

// --- in_3ch context ---
typedef struct {
    int slot_num;
    int pin_0;
    int pin_1;
    int pin_2;
    int inverse_0;
    int inverse_1;
    int inverse_2;
    int stat_0;
    int stat_1;
    int stat_2;
    int prevState_0;
    int prevState_1;
    int prevState_2;
    int debounceGap;
    int logic;                      // INDEPENDENT_MODE, OR_LOGIC_MODE, AND_LOGIC_MODE
    int stateReport_0;
    int stateReport_1;
    int stateReport_2;
    int stateReport_combined;       // For OR/AND logic
    TickType_t lastTick;
    int refreshPeriod;
} in_3ch_context_t;

#define IN_3CH_CONTEXT_DEFAULT() { \
    .slot_num = 0, \
    .pin_0 = 0, \
    .pin_1 = 0, \
    .pin_2 = 0, \
    .inverse_0 = 0, \
    .inverse_1 = 0, \
    .inverse_2 = 0, \
    .stat_0 = 0, \
    .stat_1 = 0, \
    .stat_2 = 0, \
    .prevState_0 = 0, \
    .prevState_1 = 0, \
    .prevState_2 = 0, \
    .debounceGap = 10, \
    .logic = INDEPENDENT_MODE, \
    .stateReport_0 = 0, \
    .stateReport_1 = 0, \
    .stateReport_2 = 0, \
    .stateReport_combined = 0, \
    .lastTick = 0, \
    .refreshPeriod = 100 \
}


// =============================================================================
// IN_2CH MODULE
// =============================================================================

static void IRAM_ATTR gpio_isr_handler_in_2ch(void* arg) {
    int slot_num = (int)(intptr_t)arg;
    uint8_t tmp = 1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

/* 
    Модуль двухканальный вход с поддержкой логических операций
*/
static void configure_in_2ch(in_2ch_context_t *ctx, int slot_num) {
    ctx->slot_num = slot_num;
    ctx->pin_0 = SLOTS_PIN_MAP[slot_num][0];
    ctx->pin_1 = SLOTS_PIN_MAP[slot_num][1];

    /* Инверсия сигнала на канале 0
    */
    ctx->inverse_0 = get_option_flag_val(slot_num, "inverse_0");
    ESP_LOGD(TAG, "Set inverse_0:%d for slot:%d", ctx->inverse_0, slot_num);
    
    /* Инверсия сигнала на канале 1
    */
    ctx->inverse_1 = get_option_flag_val(slot_num, "inverse_1");
    ESP_LOGD(TAG, "Set inverse_1:%d for slot:%d", ctx->inverse_1, slot_num);

    /* Время защиты от дребезга в миллисекундах
    */
    ctx->debounceGap = get_option_int_val(slot_num, "inDebounceGap", "ms", 10, 1, 4096);
    ESP_LOGD(TAG, "Set inDebounceGap:%d for slot:%d", ctx->debounceGap, slot_num);

    /* Режим логики: independent (независимые каналы), or (логическое ИЛИ), and (логическое И)
    */
    ctx->logic = get_option_enum_val(slot_num, "logic", "independent", "or", "and", NULL);
    ESP_LOGD(TAG, "Set logic:%s for slot:%d", 
             ctx->logic == OR_LOGIC_MODE ? "OR" : (ctx->logic == AND_LOGIC_MODE ? "AND" : "INDEPENDENT"), 
             slot_num);

    /* Период опроса входов в миллисекундах
    */
    ctx->refreshPeriod = get_option_int_val(slot_num, "refreshPeriod", "ms", 100, 10, 60000);
    ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d", ctx->refreshPeriod, slot_num);

    // Setup topic
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = get_option_string_val(slot_num, "topic", "/in_0");
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "Custom trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/in_0") + 3];
        sprintf(t_str, "%s/in_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    // Register ALL stdreports (избегаем ветвления при регистрации)
    char topic_0[128], topic_1[128];
    sprintf(topic_0, "%s/ch_0", me_state.trigger_topic_list[slot_num]);
    sprintf(topic_1, "%s/ch_1", me_state.trigger_topic_list[slot_num]);
    
    /* Отчет о состоянии канала 0 (0 или 1)
    */
    ctx->stateReport_0 = stdreport_register(RPTT_int, slot_num, "", topic_0);
    
    /* Отчет о состоянии канала 1 (0 или 1)
    */
    ctx->stateReport_1 = stdreport_register(RPTT_int, slot_num, "", topic_1);
    
    /* Отчет о комбинированном состоянии обоих каналов (результат логической операции OR/AND)
    */
    ctx->stateReport_combined = stdreport_register(RPTT_int, slot_num, "", me_state.trigger_topic_list[slot_num]);

    // Configure GPIO
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << ctx->pin_0) | (1ULL << ctx->pin_1);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(ctx->pin_0, gpio_isr_handler_in_2ch, (void*)(intptr_t)slot_num);
    gpio_isr_handler_add(ctx->pin_1, gpio_isr_handler_in_2ch, (void*)(intptr_t)slot_num);

    ESP_LOGD(TAG, "in_2ch configured for slot:%d, pins:%d,%d", slot_num, ctx->pin_0, ctx->pin_1);
}

static void in_2ch_task(void *arg) {
    int slot_num = (int)(intptr_t)arg;
    in_2ch_context_t ctx = IN_2CH_CONTEXT_DEFAULT();
    
    configure_in_2ch(&ctx, slot_num);
    me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));
    
    uint8_t tmp;
    ctx.lastTick = xTaskGetTickCount();

    while (1) {
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, ctx.refreshPeriod / portTICK_PERIOD_MS)) {
            vTaskDelay(ctx.debounceGap / portTICK_PERIOD_MS);
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - ctx.lastTick) * portTICK_PERIOD_MS < ctx.debounceGap) {
            continue;
        }
        ctx.lastTick = now;

        // Read current states
        ctx.stat_0 = gpio_get_level(ctx.pin_0) ? !ctx.inverse_0 : ctx.inverse_0;
        ctx.stat_1 = gpio_get_level(ctx.pin_1) ? !ctx.inverse_1 : ctx.inverse_1;

        // Check for state changes
        int changed_0 = (ctx.stat_0 != ctx.prevState_0);
        int changed_1 = (ctx.stat_1 != ctx.prevState_1);

        if (changed_0 || changed_1) {
            if (ctx.logic == INDEPENDENT_MODE) {
                // Report each channel independently
                if (changed_0) {
                    stdreport_i(ctx.stateReport_0, ctx.stat_0);
                }
                if (changed_1) {
                    stdreport_i(ctx.stateReport_1, ctx.stat_1);
                }
            } else if (ctx.logic == OR_LOGIC_MODE) {
                // OR logic: 1 if any channel is 1, 0 if all are 0
                int current_result = (ctx.stat_0 || ctx.stat_1) ? 1 : 0;
                int prev_result = (ctx.prevState_0 || ctx.prevState_1) ? 1 : 0;
                if (current_result != prev_result) {
                    stdreport_i(ctx.stateReport_combined, current_result);
                }
            } else if (ctx.logic == AND_LOGIC_MODE) {
                // AND logic: 1 only if all channels are 1
                int current_result = (ctx.stat_0 && ctx.stat_1) ? 1 : 0;
                int prev_result = (ctx.prevState_0 && ctx.prevState_1) ? 1 : 0;
                if (current_result != prev_result) {
                    stdreport_i(ctx.stateReport_combined, current_result);
                }
            }

            ctx.prevState_0 = ctx.stat_0;
            ctx.prevState_1 = ctx.stat_1;
        }
    }
}

void start_in_2ch_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    
    char tmpString[60];
    sprintf(tmpString, "task_in_2ch_%d", slot_num);
    xTaskCreatePinnedToCore(in_2ch_task, tmpString, 1024 * 5, (void*)(intptr_t)t_slot_num, configMAX_PRIORITIES - 12, NULL, 1);
    
    ESP_LOGD(TAG, "in_2ch_task created for slot: %d Heap usage: %lu free heap:%u", 
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


// =============================================================================
// IN_3CH MODULE
// =============================================================================

static void IRAM_ATTR gpio_isr_handler_in_3ch(void* arg) {
    int slot_num = (int)(intptr_t)arg;
    uint8_t tmp = 1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

/* 
    Модуль трехканальный вход с поддержкой логических операций
*/
static void configure_in_3ch(in_3ch_context_t *ctx, int slot_num) {
    ctx->slot_num = slot_num;
    ctx->pin_0 = SLOTS_PIN_MAP[slot_num][0];
    ctx->pin_1 = SLOTS_PIN_MAP[slot_num][1];
    ctx->pin_2 = SLOTS_PIN_MAP[slot_num][2];

    /* Инверсия сигнала на канале 0
    */
    ctx->inverse_0 = get_option_flag_val(slot_num, "inverse_0");
    ESP_LOGD(TAG, "Set inverse_0:%d for slot:%d", ctx->inverse_0, slot_num);
    
    /* Инверсия сигнала на канале 1
    */
    ctx->inverse_1 = get_option_flag_val(slot_num, "inverse_1");
    ESP_LOGD(TAG, "Set inverse_1:%d for slot:%d", ctx->inverse_1, slot_num);
    
    /* Инверсия сигнала на канале 2
    */
    ctx->inverse_2 = get_option_flag_val(slot_num, "inverse_2");
    ESP_LOGD(TAG, "Set inverse_2:%d for slot:%d", ctx->inverse_2, slot_num);

    /* Время защиты от дребезга в миллисекундах
    */
    ctx->debounceGap = get_option_int_val(slot_num, "inDebounceGap", "ms", 10, 1, 4096);
    ESP_LOGD(TAG, "Set inDebounceGap:%d for slot:%d", ctx->debounceGap, slot_num);

    /* Режим логики: independent (независимые каналы), or (логическое ИЛИ), and (логическое И)
    */
    ctx->logic = get_option_enum_val(slot_num, "logic", "independent", "or", "and", NULL);
    ESP_LOGD(TAG, "Set logic:%s for slot:%d", 
             ctx->logic == OR_LOGIC_MODE ? "OR" : (ctx->logic == AND_LOGIC_MODE ? "AND" : "INDEPENDENT"), 
             slot_num);

    /* Период опроса входов в миллисекундах
    */
    ctx->refreshPeriod = get_option_int_val(slot_num, "refreshPeriod", "ms", 100, 10, 60000);
    ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d", ctx->refreshPeriod, slot_num);

    // Setup topic
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = get_option_string_val(slot_num, "topic", "/in_0");
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "Custom trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/in_0") + 3];
        sprintf(t_str, "%s/in_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    // Register ALL stdreports (избегаем ветвления при регистрации)
    char topic_0[128], topic_1[128], topic_2[128];
    sprintf(topic_0, "%s/ch_0", me_state.trigger_topic_list[slot_num]);
    sprintf(topic_1, "%s/ch_1", me_state.trigger_topic_list[slot_num]);
    sprintf(topic_2, "%s/ch_2", me_state.trigger_topic_list[slot_num]);
    
    /* Отчет о состоянии канала 0 (0 или 1)
    */
    ctx->stateReport_0 = stdreport_register(RPTT_int, slot_num, "", topic_0);
    
    /* Отчет о состоянии канала 1 (0 или 1)
    */
    ctx->stateReport_1 = stdreport_register(RPTT_int, slot_num, "", topic_1);
    
    /* Отчет о состоянии канала 2 (0 или 1)
    */
    ctx->stateReport_2 = stdreport_register(RPTT_int, slot_num, "", topic_2);
    
    /* Отчет о комбинированном состоянии всех каналов (результат логической операции OR/AND)
    */
    ctx->stateReport_combined = stdreport_register(RPTT_int, slot_num, "", me_state.trigger_topic_list[slot_num]);

    // Configure GPIO
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << ctx->pin_0) | (1ULL << ctx->pin_1) | (1ULL << ctx->pin_2);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(ctx->pin_0, gpio_isr_handler_in_3ch, (void*)(intptr_t)slot_num);
    gpio_isr_handler_add(ctx->pin_1, gpio_isr_handler_in_3ch, (void*)(intptr_t)slot_num);
    gpio_isr_handler_add(ctx->pin_2, gpio_isr_handler_in_3ch, (void*)(intptr_t)slot_num);

    ESP_LOGD(TAG, "in_3ch configured for slot:%d, pins:%d,%d,%d", slot_num, ctx->pin_0, ctx->pin_1, ctx->pin_2);
}

static void in_3ch_task(void *arg) {
    int slot_num = (int)(intptr_t)arg;
    in_3ch_context_t ctx = IN_3CH_CONTEXT_DEFAULT();
    
    configure_in_3ch(&ctx, slot_num);
    
    uint8_t tmp;
    ctx.lastTick = xTaskGetTickCount();

    while (1) {
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, ctx.refreshPeriod / portTICK_PERIOD_MS)) {
            vTaskDelay(ctx.debounceGap / portTICK_PERIOD_MS);
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - ctx.lastTick) * portTICK_PERIOD_MS < ctx.debounceGap) {
            continue;
        }
        ctx.lastTick = now;

        // Read current states
        ctx.stat_0 = gpio_get_level(ctx.pin_0) ? !ctx.inverse_0 : ctx.inverse_0;
        ctx.stat_1 = gpio_get_level(ctx.pin_1) ? !ctx.inverse_1 : ctx.inverse_1;
        ctx.stat_2 = gpio_get_level(ctx.pin_2) ? !ctx.inverse_2 : ctx.inverse_2;

        // Check for state changes
        int changed_0 = (ctx.stat_0 != ctx.prevState_0);
        int changed_1 = (ctx.stat_1 != ctx.prevState_1);
        int changed_2 = (ctx.stat_2 != ctx.prevState_2);

        if (changed_0 || changed_1 || changed_2) {
            if (ctx.logic == INDEPENDENT_MODE) {
                // Report each channel independently
                if (changed_0) {
                    stdreport_i(ctx.stateReport_0, ctx.stat_0);
                }
                if (changed_1) {
                    stdreport_i(ctx.stateReport_1, ctx.stat_1);
                }
                if (changed_2) {
                    stdreport_i(ctx.stateReport_2, ctx.stat_2);
                }
            } else if (ctx.logic == OR_LOGIC_MODE) {
                // OR logic: 1 if any channel is 1, 0 if all are 0
                int current_result = (ctx.stat_0 || ctx.stat_1 || ctx.stat_2) ? 1 : 0;
                int prev_result = (ctx.prevState_0 || ctx.prevState_1 || ctx.prevState_2) ? 1 : 0;
                if (current_result != prev_result) {
                    stdreport_i(ctx.stateReport_combined, current_result);
                }
            } else if (ctx.logic == AND_LOGIC_MODE) {
                // AND logic: 1 only if all channels are 1
                int current_result = (ctx.stat_0 && ctx.stat_1 && ctx.stat_2) ? 1 : 0;
                int prev_result = (ctx.prevState_0 && ctx.prevState_1 && ctx.prevState_2) ? 1 : 0;
                if (current_result != prev_result) {
                    stdreport_i(ctx.stateReport_combined, current_result);
                }
            }

            ctx.prevState_0 = ctx.stat_0;
            ctx.prevState_1 = ctx.stat_1;
            ctx.prevState_2 = ctx.stat_2;
        }
    }
}

void start_in_3ch_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    
    char tmpString[60];
    sprintf(tmpString, "task_in_3ch_%d", slot_num);
    xTaskCreatePinnedToCore(in_3ch_task, tmpString, 1024 * 5, (void*)(intptr_t)t_slot_num, configMAX_PRIORITIES - 12, NULL, 1);
    
    ESP_LOGD(TAG, "in_3ch_task created for slot: %d Heap usage: %lu free heap:%u", 
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

// =============================================================================
// MANIFEST EXPORT
// =============================================================================

// ОБЯЗАТЕЛЬНО: Функция экспорта манифеста
const char * get_manifest_inputs()
{
    return manifesto;
}

