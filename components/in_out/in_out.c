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
    int stateReport;
    int state;
    int prevState;
    int active_state;

    // Output fields
    STDCOMMANDS cmds;
    int out_pin_num;
    int inverse_out;
    int defaultState;
    int delay;
    int impulse;
    int out_state;
    int impulseRest;        // уровень, на который вернуться по спаду импульса
    int impulsing;          // импульс сейчас активен
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
    .stateReport = 0, \
    .state = 0, \
    .prevState = 0, \
    .active_state = 1, \
    .inverse_out = 0, \
    .defaultState = 0, \
    .delay = 0, \
    .impulse = 0, \
    .out_state = 0, \
    .impulseRest = 0, \
    .impulsing = 0 \
}
/* Работаем по указателю, а не по копии структуры: out_state должен остаться
   записанным в контексте задачи. Раньше функция брала контекст ПО ЗНАЧЕНИЮ и
   правила поле в своей копии, поэтому спад импульса гасил пин, но контекст
   продолжал считать выход включённым - следующий импульс инвертировал не ту
   сторону, и команда работала через раз, как тогл. */
void set_out_level(void* arg, int level){
    in_out_context_t *ctx = (in_out_context_t*)arg;
    ctx->out_state = level;
    gpio_set_level(ctx->out_pin_num, ctx->inverse_out ? !ctx->out_state : ctx->out_state);
    //ESP_LOGD(TAG, "Set level: %d for pin: %d", level, ctx->out_pin_num);
}

/* Спад импульса выставляет ЗАПОМНЕННЫЙ уровень покоя, а не инвертирует
   текущий: инверсия врёт, если за время импульса состояние успели поменять
   командой setVal или повторным импульсом. */
void impulse_fall(void* arg){
	in_out_context_t *ctx = (in_out_context_t*)arg;
	ctx->impulsing = 0;
	set_out_level(ctx, ctx->impulseRest);
}


/* 
    Слот конфигурируется как цифровой вход/выход
    slots: 0-5
*/
void configure_in_out(in_out_context_t *ctx, int slot_num) {

    // Initialize stdcommand
    stdcommand_init(&ctx->cmds, slot_num);

    /* Старт в выключенном состоянии до action/enable 1, По умолчанию активен
    */
    ctx->active_state = !get_option_flag_val(slot_num, "disableOnStart");
    ESP_LOGD(TAG, "Initial active_state:%d for slot:%d", ctx->active_state, slot_num);

    // ========== INPUT CONFIGURATION ==========

    /* Инверсия входа
    */
    ctx->inverse_in = get_option_flag_val(slot_num, "inInverse");
    if (ctx->inverse_in) {
        ESP_LOGD(TAG, "Set inInverse for slot:%d", slot_num);
    }   

    /* Антидребезг входа в мс, По умолчанию 10
    */
    ctx->debounceGap = get_option_int_val(slot_num, "inDebounceGap", "ms", 10, 1, 4096);
    if (ctx->debounceGap!= 10) {
        ESP_LOGD(TAG, "Set inDebounceGap:%d for slot:%d", ctx->debounceGap, slot_num);
    }

    // Setup input topic
    {
        char t_str[strlen(me_config.deviceName) + strlen("/in_0") + 3];
        sprintf(t_str, "%s/in_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }

    // ========== OUTPUT CONFIGURATION ==========

    /* Инверсия выхода
    */
    ctx->inverse_out = get_option_flag_val(slot_num, "outInverse");
    if (ctx->inverse_out) {
        ESP_LOGD(TAG, "Set outInverse for slot:%d", slot_num);
    }

    /* Состояние выхода при старте 0-1, По умолчанию 0
    */
    ctx->defaultState = get_option_int_val(slot_num, "outDefaultState", "", 0, 0, 1);
    ESP_LOGD(TAG, "Set outDefaultState:%d for slot:%d", ctx->defaultState, slot_num);
    
    // Setup output topic
    {
        char t_str[strlen(me_config.deviceName) + strlen("/out_0") + 3];
        sprintf(t_str, "%s/out_%d", me_config.deviceName, slot_num);
        me_state.action_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard action_topic:%s", me_state.action_topic_list[slot_num]);
    }

    // =============================================================================
    // RAPORTS
    // =============================================================================

    /* Состояние входа 0-1
	*/
    ctx->stateReport = stdreport_register(RPTT_int, slot_num, "state", "event/val", 0, 1);


    // =============================================================================
    // COMMANDS
    // =============================================================================


    /* Установить выход 0-1
	*/
    stdcommand_register(&ctx->cmds, OUT_CMD_default, "action/setVal", PARAMT_int);

    /* Переключить выход
	*/
    stdcommand_register(&ctx->cmds, OUT_CMD_toggle, "action/toggle", PARAMT_none);

    /* Импульс на выходе - длительность в мс
	*/
    stdcommand_register(&ctx->cmds, OUT_CMD_impulse, "action/impulse", PARAMT_int);

    /* === COMMANDS === */

    /* Включить 1 или выключить 0 модуль
    */
    stdcommand_register(&ctx->cmds, STDCMD_ENABLE, "action/enable", PARAMT_int);

    /* === EVENTS === */

    /* event/enable модуль не публикует - вариант 'только команда' (Конституция §6):
       модулем управляют, но статус наружу не нужен. Наружу идёт то, ради чего
       модуль существует: event/val - уровень на ножке. */
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
    int slot_num = (int)(intptr_t)arg;

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

    // Read initial INPUT state (нужно для инициализации debounce; публикуем ниже,
    // ПОСЛЕ waitForWorkPermit - см. комментарий у отчёта).
    if (gpio_get_level(ctx.in_pin_num)) {
        ctx.state = ctx.inverse_in ? 0 : 1;
    } else {
        ctx.state = ctx.inverse_in ? 1 : 0;
    }
    ctx.prevState = ctx.state;

    // Debounce: a new level must hold continuously for debounceGap ms
    // before it is accepted as the new reported state.
    int candidate = ctx.state;          // level currently being timed
    int64_t candidate_since = 0;        // us timestamp when candidate first appeared

    esp_timer_handle_t impulse_timer;
    const esp_timer_create_args_t impulse_timer_args = {
        .callback = &impulse_fall,
        .arg = &ctx,
        .name = "impulse"
    };
    ESP_ERROR_CHECK(esp_timer_create(&impulse_timer_args, &impulse_timer));

    // Create command queue for output
    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    /* nSLEEP драйвера выходов слота - без него выход в высоком импедансе */
    enableSlotDriver(slot_num);

    waitForWorkPermit(slot_num);

    bool active_state = ctx.active_state;

    // Начальное состояние входа публикуем ТОЛЬКО здесь, после барьера. Раньше
    // отчёт стоял до waitForWorkPermit - он уходил прежде, чем у слотов-приёмников
    // кросслинка созданы очереди команд, и стартовое состояние ножки для линка
    // терялось. Перечитываем уровень - на старте ножка уже устоялась.
    if (gpio_get_level(ctx.in_pin_num)) {
        ctx.state = ctx.inverse_in ? 0 : 1;
    } else {
        ctx.state = ctx.inverse_in ? 1 : 0;
    }
    ctx.prevState = ctx.state;
    if (active_state) {
        stdreport_i(ctx.stateReport, ctx.state);
    }

    STDCOMMAND_PARAMS params = {0};

    // Main loop - handle both input and output
    for (;;) {
        uint8_t tmp;

        // Wake on an input edge, otherwise poll every 10 ms (also lets us
        // re-check a pending debounce candidate and service OUTPUT commands).
        xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, pdMS_TO_TICKS(10));

        // Read current raw INPUT level
        int raw;
        if (gpio_get_level(ctx.in_pin_num)) {
            raw = ctx.inverse_in ? 0 : 1;
        } else {
            raw = ctx.inverse_in ? 1 : 0;
        }

        int64_t now = esp_timer_get_time();

        // Restart the debounce window whenever the raw level moves.
        if (raw != candidate) {
            candidate = raw;
            candidate_since = now;
        }

        // Accept the candidate only after it has held for debounceGap ms.
        if (candidate != ctx.prevState &&
            (now - candidate_since) >= (int64_t)ctx.debounceGap * 1000) {
            ctx.state = candidate;
            ctx.prevState = candidate;

            // Send report only when enabled
            if (active_state) {
                stdreport_i(ctx.stateReport, ctx.state);
            }
        }

        // Check for OUTPUT commands (non-blocking)
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx.cmds, &params, 0);
        if(cmd>=0){
            ESP_LOGD(TAG, "Slot_%d input cmd num:%d", slot_num, cmd);
        }
        switch (cmd) {
            case STDCMD_ENABLE:
                if (params.count > 0) {
                    active_state = params.p[0].i ? 1 : 0;
                    ESP_LOGD(TAG, "enable:%d slot:%d", active_state, slot_num);
                    if (!active_state) {
                        // reset output to default state on disable
                        /* Недоигранный импульс гасим - иначе его спад перебьёт default */
                        esp_timer_stop(impulse_timer);
                        ctx.impulsing = 0;
                        ctx.out_state = ctx.defaultState;
                        set_out_level(&ctx, ctx.out_state);
                    }
                }
                break;

            case OUT_CMD_default:
                if (!active_state) break;
                ctx.out_state = params.p[0].i;
                set_out_level(&ctx, ctx.out_state);
                break;

            case OUT_CMD_toggle:
                if (!active_state) break;
                ctx.out_state = !ctx.out_state;
                set_out_level(&ctx, ctx.out_state);
                break;

            case OUT_CMD_impulse:
                if (!active_state) break;
                int length = params.p[0].i;
                /* Повторный импульс до истечения прошлого: таймер уже запущен,
                   esp_timer_start_once вернул бы ESP_ERR_INVALID_STATE, а
                   ESP_ERROR_CHECK уронил бы устройство в панику. Останавливаем
                   и отсчитываем заново; уровень покоя запоминаем только на
                   первом импульсе, иначе второй увёл бы выход в инверсию. */
                esp_timer_stop(impulse_timer);
                if (!ctx.impulsing) {
                    ctx.impulseRest = ctx.out_state;
                    ctx.impulsing = 1;
                }
                set_out_level(&ctx, !ctx.impulseRest);
                if (esp_timer_start_once(impulse_timer, (length) * 1000) != ESP_OK) {
                    ESP_LOGE(TAG, "Slot_%d: impulse timer start failed", slot_num);
                    ctx.impulsing = 0;
                    set_out_level(&ctx, ctx.impulseRest);
                }
                break;

            default:
                break;
        }

    }
}

void start_in_out_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[60];
    sprintf(tmpString, "task_in_out_%d", slot_num);
    xTaskCreatePinnedToCore(in_out_task, tmpString, 1024 * 4, (void*)(intptr_t)slot_num, 12, NULL, 1);

    ESP_LOGD(TAG, "Unified IN/OUT task created for slot: %d Heap usage: %lu free heap:%u", 
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_in_out()
{
	return manifesto;
}

