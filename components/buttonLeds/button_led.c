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
    LED_CMD_toggleLedState,
    LED_CMD_setMinBright,
    LED_CMD_setMaxBright,
    LED_CMD_setFadeTime
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

    /* Флаг определяет инверсию светодиода
    */
   ctx->led.inverse = get_option_flag_val(slot_num, "ledInverse");

   /* Интенсивность затухание свечения
   */
   ctx->led.increment = get_option_int_val(slot_num, "increment", "", 255, 1, 4096);
   ESP_LOGD(TAG, "Set increment:%d for slot:%d",ctx->led.increment, slot_num);

   /* Максимальное свечение
   */
   ctx->led.maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 4095);
   if(ctx->led.maxBright>255)ctx->led.maxBright=255;
   if(ctx->led.maxBright<0)ctx->led.maxBright=0;
   ESP_LOGD(TAG, "Set maxBright:%d for slot:%d", ctx->led.maxBright, slot_num);

   /* Минимальное свечение
   */
   ctx->led.minBright = get_option_int_val(slot_num, "minBright", "", 0, 1, 4096);
   if(ctx->led.minBright>255)ctx->led.minBright=255;
   if(ctx->led.minBright<0)ctx->led.minBright=0;
   ESP_LOGD(TAG, "Set minBright:%d for slot:%d", ctx->led.minBright, slot_num);

   /* Период обновления
   */
   ctx->led.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 40, 1, 4096));
   ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",ctx->led.refreshPeriod, slot_num);

   /* Время затухания свечения в миллисекундах
   */
   ctx->led.fadeTime = get_option_int_val(slot_num, "fadeTime", "ms", 100, 10, 10000);

   ctx->led.increment = (ctx->led.maxBright - ctx->led.minBright) * ctx->led.refreshPeriod / ctx->led.fadeTime;
   if (ctx->led.increment < 1) ctx->led.increment = 1;
   ESP_LOGD(TAG, "Calculated increment: %d for slot %d", ctx->led.increment, slot_num);

   /* Задаёт режим анимации */
   if ((ctx->led.ledMode = get_option_enum_val(slot_num, "ledMode", "none", "flash", "glitch", NULL)) < 0)
   {
       ESP_LOGE(TAG, "animate: unricognized value");
       ctx->led.ledMode = 0; // NONE
   }
   else
       ESP_LOGD(TAG, "Custom animate: %d", ctx->led.ledMode);

   /* Состояние по умолчанию
   */
   ctx->led.state = get_option_int_val(slot_num, "defaultState", "", 0, 0, 1) ^ ctx->led.inverse;

   if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
       char* custom_topic=NULL;
       /* Топик для режима свечения
       */
       custom_topic = get_option_string_val(slot_num, "ledTopic", "/led_0");
       me_state.action_topic_list[slot_num]=strdup(custom_topic);
       ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
   }else{
       char t_str[strlen(me_config.deviceName)+strlen("/led_0")+3];
       sprintf(t_str, "%s/led_%d",me_config.deviceName, slot_num);
       me_state.action_topic_list[slot_num]=strdup(t_str);
       ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
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

    
    /* задаёт текущее состояние светодиода (вкл/выкл)
    Числовое значение 0-1
    */
    stdcommand_register(&ctx->led.cmds, LED_CMD_default, NULL, PARAMT_int);

    /* Команда меняет текущее состояние светодиода на противоположное
    */
    stdcommand_register(&ctx->led.cmds, LED_CMD_toggleLedState, "toggleLedState", PARAMT_none);

    /* Установить минимальное значение яркости
    */
    stdcommand_register(&ctx->led.cmds, LED_CMD_setMinBright, "setMinBright", PARAMT_int);

    /* Установить максимальное значение яркости
    */
    stdcommand_register(&ctx->led.cmds, LED_CMD_setMaxBright, "setMaxBright", PARAMT_int);

    /* Установить время переходного процесса при изменении яркомсти в миллесекндах
    */
    stdcommand_register(&ctx->led.cmds, LED_CMD_setFadeTime, "setFadeTime", PARAMT_int);
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
            case LED_CMD_setMinBright:
                ctx->led.minBright = params.p[0].i;
                if(ctx->led.minBright > 255) ctx->led.minBright = 255;
                if(ctx->led.minBright < 0) ctx->led.minBright = 0;
                targetBright = ctx->led.state ? ctx->led.maxBright : ctx->led.minBright;
                ESP_LOGD(TAG, "Set minBright:%d for slot:%d", ctx->led.minBright, slot_num);
                break;
            case LED_CMD_setMaxBright:
                ctx->led.maxBright = params.p[0].i;
                if(ctx->led.maxBright > 255) ctx->led.maxBright = 255;
                if(ctx->led.maxBright < 0) ctx->led.maxBright = 0;
                targetBright = ctx->led.state ? ctx->led.maxBright : ctx->led.minBright;
                ESP_LOGD(TAG, "Set maxBright:%d for slot:%d", ctx->led.maxBright, slot_num);
                break;
            case LED_CMD_setFadeTime:
                ctx->led.fadeTime = params.p[0].i;
                ctx->led.increment = (ctx->led.maxBright - ctx->led.minBright) * ctx->led.refreshPeriod / ctx->led.fadeTime;
                if (ctx->led.increment < 1) ctx->led.increment = 1;
                ESP_LOGD(TAG, "Set fadeTime:%d, recalculated increment:%d for slot:%d", ctx->led.fadeTime, ctx->led.increment, slot_num);
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