// ***************************************************************************
// TITLE: Multi-Channel Output Modules (out_2ch, out_3ch)
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
#include "stdcommand.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "IN_OUT";

#include <generated_files/gen_outputs.h>


typedef struct {
    // Input fields
    
    // Output fields
    STDCOMMANDS cmds;
    int out_pinMass[3];
    int inverseMass[3];
    int defaultStateMass[3];
    int stateMass[3];
    int numOfCh;
} out_context_t;


typedef enum
{
    OUT_CMD_ch_0_default = 0,
    OUT_CMD_ch_1_default,
    OUT_CMD_ch_2_default, 
    OUT_CMD_ch_0_toggle,
    OUT_CMD_ch_1_toggle,
    OUT_CMD_ch_2_toggle,
    OUT_CMD_ch_0_impulse,
    OUT_CMD_ch_1_impulse,
    OUT_CMD_ch_2_impulse,
} OUT_CMD;


void _set_out_level(void* arg, int level, int index){
    out_context_t *ctx = (out_context_t*)arg;
    ctx->stateMass[index] = level;
    gpio_set_level(ctx->out_pinMass[index], ctx->inverseMass[index] ? !ctx->stateMass[index] : ctx->stateMass[index]);
    // ESP_LOGD(TAG, "Set level: %d on pin:%d (inverse:%d, gpio_level:%d)", 
    //          ctx->stateMass[index], ctx->out_pinMass[index], ctx->inverseMass[index],
    //          ctx->inverseMass[index] ? !ctx->stateMass[index] : ctx->stateMass[index]);
}

void _impulse_fall_0(void* arg){
	out_context_t *ctx = (out_context_t*)arg;
	_set_out_level(ctx, !ctx->stateMass[0], 0);
	// ESP_LOGD(TAG, "impulse fall ch_0: Set level: %d ", ctx->stateMass[0]);
}

void _impulse_fall_1(void* arg){
	out_context_t *ctx = (out_context_t*)arg;
	_set_out_level(ctx, !ctx->stateMass[1], 1);
	// ESP_LOGD(TAG, "impulse fall ch_1: Set level: %d ", ctx->stateMass[1]);
}

void _impulse_fall_2(void* arg){
	out_context_t *ctx = (out_context_t*)arg;
	_set_out_level(ctx, !ctx->stateMass[2], 2);
	// ESP_LOGD(TAG, "impulse fall ch_2: Set level: %d ", ctx->stateMass[2]);
}


/* Слот конфигурируется как два цифровых выхода
*/
void configure_out_2ch(out_context_t *ctx, int slot_num) {
    ctx->numOfCh = 2;

    // Initialize stdcommand
    stdcommand_init(&ctx->cmds, slot_num);

    /* Настраивает инверсию выходного сигнала для канала 0
    0-1 по умолчанию 0
    */
    ctx->inverseMass[0] = get_option_int_val(slot_num, "inverse_0", "bool",0, 0, 1);
    
    /* Настраивает инверсию выходного сигнала для канала 1
    0-1 по умолчанию 0
    */
    ctx->inverseMass[1] = get_option_int_val(slot_num, "inverse_1","bool",0, 0, 1);

    /* Настраивает значение по умолчанию для канало 0
    0-1 поумолчанию 0
    */
    ctx->defaultStateMass[0] = get_option_int_val(slot_num, "defState_0", "bool", 0, 0, 1);
    
    /* Настраивает значение по умолчанию для канало 1
    0-1 поумолчанию 0
    */
    ctx->defaultStateMass[1] = get_option_int_val(slot_num, "defState_1", "bool", 0, 0, 1);

    // Setup topic
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        /* Пользовательская топик для выходного сигнала
        */
        char* custom_topic = get_option_string_val(slot_num, "topic", "/out_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "Custom action_topic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/out_0") + 3];
        sprintf(t_str, "%s/out_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard action_topic:%s", me_state.action_topic_list[slot_num]);
    }

    // Register commands
    /* Команда для установки состояния выходного сигнала канала 0
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_0_default, "ch_0", PARAMT_int);
    /* Команда для установки состояния выходного сигнала канала 1
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_1_default, "ch_1", PARAMT_int);

    /* Команда для переключения состояния выходного сигнала канала 0
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_0_toggle, "ch_0/toggle", PARAMT_none);

    /* Команда для переключения состояния выходного сигнала канала 1
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_1_toggle, "ch_1/toggle", PARAMT_none);

    /* Команда для импульсного включения выходного сигнала канала 0
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_0_impulse, "ch_0/impulse", PARAMT_int);

    /* Команда для импульсного включения выходного сигнала канала 1
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_1_impulse, "ch_1/impulse", PARAMT_int);
}




static void out_2ch_task(void *arg) {
    
    int slot_num =  *(int*)arg;

    out_context_t ctx;
    configure_out_2ch(&ctx, slot_num);

    // Configure all GPIO pins
    for (int i = 0; i < ctx.numOfCh; i++) {
        ctx.out_pinMass[i] = SLOTS_PIN_MAP[slot_num][i];
        esp_rom_gpio_pad_select_gpio(ctx.out_pinMass[i]);
        gpio_set_direction(ctx.out_pinMass[i], GPIO_MODE_OUTPUT);
        ESP_LOGD(TAG, "SETUP OUT_pin_%d Slot:%d", ctx.out_pinMass[i], slot_num);
    }

    // Set default states
    vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < ctx.numOfCh; i++) {
        ctx.stateMass[i] = ctx.defaultStateMass[i];
        uint8_t level = ctx.inverseMass[i] ? !ctx.stateMass[i] : ctx.stateMass[i];
        gpio_set_level(ctx.out_pinMass[i], level);
    }

    // Create command queue
    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    esp_timer_handle_t impulse_timer_0;
    const esp_timer_create_args_t impulse_timer_0_args = {
        .callback = &_impulse_fall_0,
        .arg = &ctx,
        .name = "impulse_0"
    };
    esp_timer_create(&impulse_timer_0_args, &impulse_timer_0);

    esp_timer_handle_t impulse_timer_1;
    const esp_timer_create_args_t impulse_timer_1_args = {
        .callback = &_impulse_fall_1,
        .arg = &ctx,
        .name = "impulse_1"
    };
    esp_timer_create(&impulse_timer_1_args, &impulse_timer_1);

    waitForWorkPermit(slot_num);
    STDCOMMAND_PARAMS params = {0};

    while (1) {
        // Check for OUTPUT commands (non-blocking)
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx.cmds, &params, portMAX_DELAY);
        if(cmd>=0){
            ESP_LOGD(TAG, "Slot_%d input cmd_num:%d val:%ld ", slot_num, cmd, params.p[0].i);
        }
        switch (cmd) {
            case OUT_CMD_ch_0_default:
                // ESP_LOGD(TAG, "OUT_CMD_ch_0_default val:%ld ", params.p[0].i);
                _set_out_level(&ctx, params.p[0].i, 0); 
                break;

            case OUT_CMD_ch_1_default:
                // ESP_LOGD(TAG, "OUT_CMD_ch_1_default val:%ld ", params.p[0].i);
                _set_out_level(&ctx, params.p[0].i, 1);
                break;

            case OUT_CMD_ch_0_toggle:
                _set_out_level(&ctx, !ctx.stateMass[0], 0); 
                break;

            case OUT_CMD_ch_1_toggle:
                _set_out_level(&ctx, !ctx.stateMass[1], 1);
                break;

            case OUT_CMD_ch_0_impulse:
                int length_0 = params.p[0].i;
                _set_out_level(&ctx, !ctx.stateMass[0], 0); 
                ESP_ERROR_CHECK(esp_timer_start_once(impulse_timer_0, (length_0) * 1000));
                // ESP_LOGD(TAG, "Started impulse_timer_0 for %d ms", length_0);
                break; 

            case OUT_CMD_ch_1_impulse:
                int length_1 = params.p[0].i;
                _set_out_level(&ctx, !ctx.stateMass[1], 1); 
                ESP_ERROR_CHECK(esp_timer_start_once(impulse_timer_1, (length_1) * 1000));
                break;

            default:
                break;
        }
        
    }
}

void start_out_2ch_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "task_out_2ch_%d", slot_num);
    xTaskCreatePinnedToCore(out_2ch_task, tmpString, 1024 * 5, &slot_num, 12, NULL, 1);

    ESP_LOGD(TAG, "Out_2ch task created for slot: %d Heap usage: %lu free heap:%u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}



/**
 * @brief Configure 3-channel output module
 * @manifest configure 3-channel output parameters from slot options
 */
void configure_out_3ch(out_context_t *ctx, int slot_num) {
    ctx->numOfCh = 3;

    // Initialize stdcommand
    stdcommand_init(&ctx->cmds, slot_num);

    /* Настраивает инверсию выходного сигнала для канала 0
    0-1 по умолчанию 0
    */
    ctx->inverseMass[0] = get_option_int_val(slot_num, "inverse_0", "bool", 0, 0, 1);
    
    /* Настраивает инверсию выходного сигнала для канала 1
    0-1 по умолчанию 0
    */
    ctx->inverseMass[1] = get_option_int_val(slot_num, "inverse_1", "bool", 0, 0, 1);

    /* Настраивает инверсию выходного сигнала для канала 2
    0-1 по умолчанию 0
    */
    ctx->inverseMass[2] = get_option_int_val(slot_num, "inverse_2", "bool", 0, 0, 1);

    /* Настраивает значение по умолчанию для канала 0
    0-1 по умолчанию 0
    */
    ctx->defaultStateMass[0] = get_option_int_val(slot_num, "defState_0", "bool", 0, 0, 1);
    
    /* Настраивает значение по умолчанию для канала 1
    0-1 по умолчанию 0
    */
    ctx->defaultStateMass[1] = get_option_int_val(slot_num, "defState_1", "bool", 0, 0, 1);

    /* Настраивает значение по умолчанию для канала 2
    0-1 по умолчанию 0
    */
    ctx->defaultStateMass[2] = get_option_int_val(slot_num, "defState_2", "bool", 0, 0, 1);

    // Setup topic
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        /* Пользовательский топик для выходного сигнала
        */
        char* custom_topic = get_option_string_val(slot_num, "topic", "/out_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "Custom action_topic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/out_0") + 3];
        sprintf(t_str, "%s/out_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard action_topic:%s", me_state.action_topic_list[slot_num]);
    }

    // Register commands
    /* Команда для установки состояния выходного сигнала канала 0
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_0_default, "ch_0", PARAMT_int);
    
    /* Команда для установки состояния выходного сигнала канала 1
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_1_default, "ch_1", PARAMT_int);
    
    /* Команда для установки состояния выходного сигнала канала 2
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_2_default, "ch_2", PARAMT_int);

    /* Команда для переключения состояния выходного сигнала канала 0
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_0_toggle, "ch_0/toggle", PARAMT_none);

    /* Команда для переключения состояния выходного сигнала канала 1
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_1_toggle, "ch_1/toggle", PARAMT_none);

    /* Команда для переключения состояния выходного сигнала канала 2
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_2_toggle, "ch_2/toggle", PARAMT_none);

    /* Команда для импульсного включения выходного сигнала канала 0
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_0_impulse, "ch_0/impulse", PARAMT_int);

    /* Команда для импульсного включения выходного сигнала канала 1
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_1_impulse, "ch_1/impulse", PARAMT_int);

    /* Команда для импульсного включения выходного сигнала канала 2
    */
    stdcommand_register(&ctx->cmds, OUT_CMD_ch_2_impulse, "ch_2/impulse", PARAMT_int);
}

static void out_3ch_task(void *arg) {
    
    int slot_num = *(int*)arg;

    out_context_t ctx;
    configure_out_3ch(&ctx, slot_num);

    // Configure all GPIO pins
    for (int i = 0; i < ctx.numOfCh; i++) {
        ctx.out_pinMass[i] = SLOTS_PIN_MAP[slot_num][i];
        esp_rom_gpio_pad_select_gpio(ctx.out_pinMass[i]);
        gpio_set_direction(ctx.out_pinMass[i], GPIO_MODE_OUTPUT);
        ESP_LOGD(TAG, "SETUP OUT_pin_%d Slot:%d", ctx.out_pinMass[i], slot_num);
    }

    // Set default states
    vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < ctx.numOfCh; i++) {
        ctx.stateMass[i] = ctx.defaultStateMass[i];
        uint8_t level = ctx.inverseMass[i] ? !ctx.stateMass[i] : ctx.stateMass[i];
        gpio_set_level(ctx.out_pinMass[i], level);
    }

    // Create command queue
    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    // Create impulse timers for all 3 channels
    esp_timer_handle_t impulse_timer_0;
    const esp_timer_create_args_t impulse_timer_0_args = {
        .callback = &_impulse_fall_0,
        .arg = &ctx,
        .name = "impulse_0"
    };
    esp_timer_create(&impulse_timer_0_args, &impulse_timer_0);

    esp_timer_handle_t impulse_timer_1;
    const esp_timer_create_args_t impulse_timer_1_args = {
        .callback = &_impulse_fall_1,
        .arg = &ctx,
        .name = "impulse_1"
    };
    esp_timer_create(&impulse_timer_1_args, &impulse_timer_1);

    esp_timer_handle_t impulse_timer_2;
    const esp_timer_create_args_t impulse_timer_2_args = {
        .callback = &_impulse_fall_2,
        .arg = &ctx,
        .name = "impulse_2"
    };
    esp_timer_create(&impulse_timer_2_args, &impulse_timer_2);

    waitForWorkPermit(slot_num);

    while (1) {
        // Check for OUTPUT commands
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx.cmds, &params, portMAX_DELAY);
        if(cmd >= 0){
            ESP_LOGD(TAG, "Slot_%d input cmd_num:%d val:%ld", slot_num, cmd, params.p[0].i);
        }
        
        switch (cmd) {
            case OUT_CMD_ch_0_default:
                _set_out_level(&ctx, params.p[0].i, 0); 
                break;

            case OUT_CMD_ch_1_default:
                _set_out_level(&ctx, params.p[0].i, 1);
                break;

            case OUT_CMD_ch_2_default:
                _set_out_level(&ctx, params.p[0].i, 2);
                break;

            case OUT_CMD_ch_0_toggle:
                _set_out_level(&ctx, !ctx.stateMass[0], 0); 
                break;

            case OUT_CMD_ch_1_toggle:
                _set_out_level(&ctx, !ctx.stateMass[1], 1);
                break;

            case OUT_CMD_ch_2_toggle:
                _set_out_level(&ctx, !ctx.stateMass[2], 2);
                break;

            case OUT_CMD_ch_0_impulse:
                int length_0 = params.p[0].i;
                _set_out_level(&ctx, !ctx.stateMass[0], 0); 
                ESP_ERROR_CHECK(esp_timer_start_once(impulse_timer_0, (length_0) * 1000));
                break; 

            case OUT_CMD_ch_1_impulse:
                int length_1 = params.p[0].i;
                _set_out_level(&ctx, !ctx.stateMass[1], 1); 
                ESP_ERROR_CHECK(esp_timer_start_once(impulse_timer_1, (length_1) * 1000));
                break;

            case OUT_CMD_ch_2_impulse:
                int length_2 = params.p[0].i;
                _set_out_level(&ctx, !ctx.stateMass[2], 2); 
                ESP_ERROR_CHECK(esp_timer_start_once(impulse_timer_2, (length_2) * 1000));
                break;

            default:
                break;
        }
    }
}

void start_out_3ch_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "task_out_3ch_%d", slot_num);
    xTaskCreatePinnedToCore(out_3ch_task, tmpString, 1024 * 5, &slot_num, 12, NULL, 1);

    ESP_LOGD(TAG, "Out_3ch task created for slot: %d Heap usage: %lu free heap:%u",
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

// =============================================================================
// MANIFEST FUNCTIONS
// =============================================================================

const char * get_manifest_outputs()
{
	return manifesto;
}