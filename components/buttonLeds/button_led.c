// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "buttonLeds.h"
#include "button_logic.h"
#include "led_types.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "stateConfig.h"
#include "executor.h"
#include "stdcommand.h"
#include "stdreport.h"
#include "math.h"
#include <arsenal.h>

#include <generated_files/gen_button_led.h>

static const char *TAG = "BUTTON_LEDS";
#undef  LOG_LOCAL_LEVEL 
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

// --- button_led ---
typedef enum
{
    LED_CMD_default = 0,
    LED_CMD_toggleLedState
} LED_CMD;

/* 
    Модуль кнопка с обычным светодиодом
*/
void configure_button_led(PMODULE_CONTEXT ctx, int slot_num)
{
    stdcommand_init(&ctx->led.cmds, slot_num);

    // --- Button logic config ---
    /* Флаг определяет инверсию кнопки
    */
    ctx->button.button_inverse = get_option_flag_val(slot_num, "buttonInverse");

    /* Глубина фильтра от дребезга контактов
    */
    ctx->button.debounce_gap = get_option_int_val(slot_num, "buttonDebounceGap", "", 10, 1, 4096);

    /* Продолжительность длинного нажатия
    - при значении 0 функция не активна
    */
    ctx->button.longPressTime 	= get_option_int_val(slot_num, "longPressTime", "ms", 0, 0, 65535);

    /* Длительность промежутка между нажатиями для регистрации двойного нажатия
    */
    ctx->button.doubleClickTime = get_option_int_val(slot_num, "doubleClickTime", "ms", 0, 0, 65535);

    /* Флаг задаёт фильтрацию совытий при активных
    */
    ctx->button.event_filter = get_option_flag_val(slot_num, "eventFilter");

    /* Период обновления потока
    */
    ctx->button.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "hz", 40, 1, 4096));

    if (strstr(me_config.slot_options[slot_num], "buttonTopic") != NULL) 
    {
        /* Топик для событий кнопки
        */
        char * custom_topic = get_option_string_val(slot_num, "buttonTopic", "/button_0");
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
    }else{
        char t_str[strlen(me_config.deviceName)+strlen("/button_0")+3];
        sprintf(t_str, "%s/button_%d",me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
    }

	/* Рапортует при изменении состояния кнопки
	*/
	ctx->button.stateReport = stdreport_register(RPTT_int, slot_num, "state", nil, 0, 1);

	/* Рапортует при регистрации длинного нажатия
	*/
	ctx->button.longReport = stdreport_register(RPTT_int, slot_num, "state", "longPress", 0, 1);

	/* Рапортует при регистрации двойного нажатия
	*/
	ctx->button.doubleReport = stdreport_register(RPTT_int, slot_num, "state", "doubleClick", 0, 1);

    // --- LED logic config ---
    configure_led_basic(&ctx->led, slot_num);
    
    /* задаёт текущее состояние светодиода (вкл/выкл)
    Числовое значение 0-1
    */
    stdcommand_register(&ctx->led.cmds, LED_CMD_default, NULL, PARAMT_int);

    /* Команда меняет текущее состояние светодиода на противоположное
    */
    stdcommand_register(&ctx->led.cmds, LED_CMD_toggleLedState, "toggleLedState", PARAMT_none);
}

void button_led_task(void *arg)
{
    int slot_num = *(int*)arg;
    PMODULE_CONTEXT ctx = calloc(1, sizeof(MODULE_CONTEXT));
    setup_button_hw(slot_num, ctx);
    configure_button_led(ctx, slot_num);

    uint8_t pin_in = SLOTS_PIN_MAP[slot_num][0];
    uint8_t pin_out = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    int channel = get_next_ledc_channel();
    if (channel < 0) {
        free(ctx);
        vTaskDelete(NULL);
    }

    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 4000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = (ledc_channel_t)channel,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = pin_out,
        .duty           = 0,
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);

    int prev_button_state = -1;
    int16_t currentBright = 0;
    int16_t appliedBright = -1;
    int16_t targetBright = ctx->led.state ? ctx->led.maxBright : ctx->led.minBright;
    bool brightnessCompletedLogged = false;

    waitForWorkPermit(slot_num);
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx->led.cmds, &params, 0);
        if(cmd>=0){
            ESP_LOGD(TAG, "Slot_%d input cmd num:%d", slot_num, cmd);
        }
        switch (cmd) {
            case LED_CMD_default:
                ctx->led.state = params.p[0].i ^ ctx->led.inverse;
                targetBright = ctx->led.state ? ctx->led.maxBright : ctx->led.minBright;
                break;
            case LED_CMD_toggleLedState:
                ctx->led.state = !ctx->led.state;
                targetBright = ctx->led.state ? ctx->led.maxBright : ctx->led.minBright;
                break;
        }

        uint8_t msg;
        int button_raw = gpio_get_level(pin_in);
        if (xQueueReceive(me_state.interrupt_queue[slot_num], &msg, 0) == pdPASS) {
            if (ctx->button.debounce_gap > 0) vTaskDelay(ctx->button.debounce_gap);
            button_raw = gpio_get_level(pin_in);
        }
        int button_state = (ctx->button.button_inverse ? !button_raw : button_raw);
        button_logic_update(&ctx->button, button_state, slot_num, &prev_button_state);

        update_led_basic(&ctx->led, &ledc_channel, &currentBright, &appliedBright, &targetBright);

        if (currentBright == targetBright) {
            if (!brightnessCompletedLogged) {
                ESP_LOGD(TAG, "Brightness change completed for slot %d", slot_num);
                brightnessCompletedLogged = true;
            }
        } else {
            brightnessCompletedLogged = false;
        }

        vTaskDelayUntil(&lastWakeTime, ctx->button.refreshPeriod);
    }
}

void start_button_led_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_button_led_%d", slot_num);
    xTaskCreate(button_led_task, tmpString, 1024*4, &slot_num, configMAX_PRIORITIES-5, NULL);
}

const char * get_manifest_button_led()
{
	return manifesto;
}