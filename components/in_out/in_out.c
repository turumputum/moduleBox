#include <stdio.h>
#include "in_out.h"

// ***************************************************************************
// TITLE: Single Channel Input/Output Module
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
#include <arsenal.h>
#include "stdcommand.h"
#include "stdreport.h"

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

#include <generated_files/gen_in_out.h>

// =============================================================================
// CONFIGURATION FUNCTIONS
// =============================================================================

/**
 * @brief Unified configuration for input/output module
 * @manifest configure input and output parameters from slot options
 */
typedef struct {
    // Input fields
    int inverse_in;
    int in_pin_num;
    int reportDelay;
    int debounceGap;
    void* stateReport;
    int state;
    int prevState;
    
    // Output fields
    STDCOMMANDS cmds;
    int out_pin_num;
    int inverse_out;
    int defaultState;
    int delay;
    int impulse;
    int out_state;
} in_out_context_t;

// --- button_led ---
typedef enum
{
    OUT_CMD_default = 0,
    OUT_CMD_toggle,
    OUT_CMD_impulse,
} OUT_CMD;

#define IN_OUT_CONTEXT_DEFAULT() { \
    .inverse_in = 0, \
    .reportDelay = 0, \
    .debounceGap = 0, \
    .stateReport = NULL, \
    .state = 0, \
    .prevState = 0, \
    .inverse_out = 0, \
    .defaultState = 0, \
    .delay = 0, \
    .impulse = 0, \
    .out_state = 0 \
}
void set_out_level(void* arg, int level){
    in_out_context_t ctx = *(in_out_context_t*)arg;
    ctx.out_state = level;
    gpio_set_level(ctx.out_pin_num, ctx.inverse_out ? !ctx.out_state : ctx.out_state);
    //ESP_LOGD(TAG, "Set level: %ld for slot: %d", cmd.level, cmd.slot_num);
}

void impulse_fall(void* arg){
	in_out_context_t ctx = *(in_out_context_t*)arg;
	set_out_level(arg, !ctx.out_state);
	//ESP_LOGD(TAG, "Set level: %ld for slot: %d", cmd.level, cmd.slot_num);
}


/* Слот конфигурируется как цифровой вход/выход
*/
void configure_in_out(in_out_context_t *ctx, int slot_num) {

    // Initialize stdcommand
    stdcommand_init(&ctx->cmds, slot_num);

    // ========== INPUT CONFIGURATION ==========
    
    /* Флаг определяет инверсию входного сигнала
    */
    ctx->inverse_in = get_option_flag_val(slot_num, "inInverse");
    if (ctx->inverse_in) {
        ESP_LOGD(TAG, "Set inInverse for slot:%d", slot_num);
    }   

    /* Глубина фильтра от дребезга контактов
    */
    ctx->debounceGap = get_option_int_val(slot_num, "inDebounceGap", "", 10, 1, 4096);
    if (ctx->debounceGap!= 10) {
        ESP_LOGD(TAG, "Set inDebounceGap:%d for slot:%d", ctx->debounceGap, slot_num);
    }

    // Setup input topic
    if (strstr(me_config.slot_options[slot_num], "inTopic") != NULL) {
        /* Пользовательская топик для входного сигнала
        */
        char* custom_topic = get_option_string_val(slot_num, "inTopic", "/in_0");
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "Custom trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/in_0") + 3];
        sprintf(t_str, "%s/in_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    // ========== OUTPUT CONFIGURATION ==========

    /* Флаг определяет инверсию выходного сигнала
    */
    ctx->inverse_out = get_option_flag_val(slot_num, "outInverse");
    if (ctx->inverse_out) {
        ESP_LOGD(TAG, "Set outInverse for slot:%d", slot_num);
    }

    /* Определяет состояние выхода при старте устройства
    0 - LOW, 1 - HIGH, по умолчанию 0
    */
    ctx->defaultState = get_option_int_val(slot_num, "outDefaultState", "", 0, 0, 1);
    ESP_LOGD(TAG, "Set outDefaultState:%d for slot:%d", ctx->defaultState, slot_num);
    
    // Setup output topic
    if (strstr(me_config.slot_options[slot_num], "outTopic") != NULL) {
        /* Пользовательская топик для выходного сигнала
        */
        char* custom_topic = get_option_string_val(slot_num, "outTopic", "/out_0");
        me_state.action_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "Custom action_topic:%s", me_state.action_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/out_0") + 3];
        sprintf(t_str, "%s/out_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard action_topic:%s", me_state.action_topic_list[slot_num]);
    }

    // =============================================================================
    // RAPORTS
    // =============================================================================

    /* Рапортует при изменении состояния входного сигнала
	*/
    ctx->stateReport = stdreport_register(RPTT_int, slot_num, "state", nil, 0, 1);


    // =============================================================================
    // COMMANDS
    // =============================================================================


    /* Команда для установки состояния выходного сигнала
	*/
    stdcommand_register(&ctx->cmds, OUT_CMD_default, NULL, PARAMT_int);

    /* Команда для переключения состояния выходного сигнала
	*/
    stdcommand_register(&ctx->cmds, OUT_CMD_toggle, "toggle", PARAMT_none);

    /* Команда формирование импульса, длинна импульса задается занчением в миллисекундах
	*/
    stdcommand_register(&ctx->cmds, OUT_CMD_impulse, "impulse", PARAMT_none);
}


// =============================================================================
// GPIO INTERRUPT HANDLERS
// =============================================================================

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    int slot_num = (int)arg;
    uint8_t tmp = 1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

// static void gpio_handler(void* arg) {
//     int slot_num = (int)arg;
//     uint8_t tmp = 1;
//     xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
// }

// =============================================================================
// UNIFIED INPUT/OUTPUT TASK
// =============================================================================

static void in_out_task(void *arg) {
    int slot_num = *(int*)arg;

    // Create interrupt queue for input
    me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));

    // Configure unified context
    in_out_context_t ctx = IN_OUT_CONTEXT_DEFAULT();
    configure_in_out(&ctx, slot_num);

    
    ctx.in_pin_num = SLOTS_PIN_MAP[slot_num][0];
    ctx.out_pin_num = SLOTS_PIN_MAP[slot_num][1];

    // Configure INPUT GPIO
    gpio_reset_pin(ctx.in_pin_num);
    esp_rom_gpio_pad_select_gpio(ctx.in_pin_num);
    gpio_config_t in_conf = {};
    in_conf.intr_type = GPIO_INTR_ANYEDGE;
    in_conf.pin_bit_mask = (1ULL << ctx.in_pin_num);
    in_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&in_conf);
    gpio_set_intr_type(ctx.in_pin_num, GPIO_INTR_ANYEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ctx.in_pin_num, gpio_isr_handler, (void*)slot_num);

    // Configure OUTPUT GPIO
    esp_rom_gpio_pad_select_gpio(ctx.out_pin_num);
    gpio_set_direction(ctx.out_pin_num, GPIO_MODE_OUTPUT);

    ESP_LOGD(TAG, "SETUP IN_pin_%d OUT_pin_%d Slot:%d", ctx.in_pin_num, ctx.out_pin_num, slot_num);

    

    // Set default OUTPUT state
    ctx.out_state = ctx.defaultState;
    set_out_level(&ctx, ctx.out_state);  

    // Read initial INPUT state
    uint32_t tick = xTaskGetTickCount();
    if (gpio_get_level(ctx.in_pin_num)) {
        ctx.state = ctx.inverse_in ? 0 : 1;
    } else {
        ctx.state = ctx.inverse_in ? 1 : 0;
    }
    
    // Report initial input state
    stdreport_i(ctx.stateReport, ctx.state);
    ctx.prevState = ctx.state;

    // Create debounce timer
    esp_timer_handle_t debounce_gap_timer;
    const esp_timer_create_args_t delay_timer_args = {
        .callback = &gpio_isr_handler,
        .arg = (void*)slot_num,
        .name = "debounce_gap_timer"
    };
    esp_timer_create(&delay_timer_args, &debounce_gap_timer);


    esp_timer_handle_t impulse_timer;
    const esp_timer_create_args_t impulse_timer_args = {
        .callback = &impulse_fall,
        .arg = &ctx,
        .name = "impulse"
    };
    ESP_ERROR_CHECK(esp_timer_create(&impulse_timer_args, &impulse_timer));

    // Create command queue for output
    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    waitForWorkPermit(slot_num);

    STDCOMMAND_PARAMS params = {0};

    // Main loop - handle both input and output
    for (;;) {
        uint8_t tmp;
        
        // Check for INPUT events (with timeout to also handle OUTPUT commands)
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, pdMS_TO_TICKS(10)) == pdPASS) {
            // Read current INPUT state
            if (gpio_get_level(ctx.in_pin_num)) {
                ctx.state = ctx.inverse_in ? 0 : 1;
            } else {
                ctx.state = ctx.inverse_in ? 1 : 0;
            }

            // Check debounce
            if (ctx.debounceGap != 0) {
                if ((xTaskGetTickCount() - tick) < ctx.debounceGap) {
                    continue;
                }
            }

            // Check state change
            if (ctx.state != ctx.prevState) {
                ctx.prevState = ctx.state;

                // Send report
                stdreport_i(ctx.stateReport, ctx.state);

                tick = xTaskGetTickCount();
                if (ctx.debounceGap != 0) {
                    esp_timer_start_once(debounce_gap_timer, ctx.debounceGap * 1000);
                }
            }
        }

        // Check for OUTPUT commands (non-blocking)
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx.cmds, &params, 0);
        if(cmd>=0){
            ESP_LOGD(TAG, "Slot_%d input cmd num:%d", slot_num, cmd);
        }
        switch (cmd) {
            case OUT_CMD_default:
                ctx.out_state = params.p[0].i;
                set_out_level(&ctx, ctx.out_state); 
                break;

            case OUT_CMD_toggle:
                ctx.out_state = !ctx.out_state;
                set_out_level(&ctx, ctx.out_state); 
                break;

            case OUT_CMD_impulse:
                int length = params.p[0].i;
                ctx.out_state = !ctx.out_state;
                set_out_level(&ctx, ctx.out_state); 
                ESP_ERROR_CHECK(esp_timer_start_once(impulse_timer, (length) * 1000));

            default:
                break;
        }
        
    }
}

void start_in_out_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    char tmpString[60];
    sprintf(tmpString, "task_in_out_%d", slot_num);
    xTaskCreatePinnedToCore(in_out_task, tmpString, 1024 * 4, &t_slot_num, 12, NULL, 1);

    ESP_LOGD(TAG, "Unified IN/OUT task created for slot: %d Heap usage: %lu free heap:%u", 
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_in_out()
{
	return manifesto;
}

