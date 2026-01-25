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

#include <generated_files/gen_button_smartLed.h>

static const char *TAG = "BUTTON_LEDS";
#undef  LOG_LOCAL_LEVEL 
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern const uint8_t gamma_8[256];
// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

// --- button_smartLed ---
typedef enum
{
    SMARTLED_default = 0,
    SMARTLED_toggleLedState,
    SMARTLED_setRGB,
    SMARTLED_setMode,
    SMARTLED_setFadeTime,
} SMARTLED_CMD;


typedef enum
{
    SMARTLED_MODE_default = 0,
    SMARTLED_MODE_flash,
    SMARTLED_MODE_rainbow,

} SMARTLED_MODE;

/* 
    Модуль кнопка со смарт-светодиодами
*/
void configure_button_smartLed(PMODULE_CONTEXT ctx, int slot_num)
{
    stdcommand_init(&ctx->led.cmds, slot_num);

    // --- Button logic config ---
    /* Флаг определяет инверсию кнопки
    */
    ctx->button.button_inverse = get_option_flag_val(slot_num, "buttonInverse");

    /* Глубина фильтра от дребезга
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

    /* Период обновления
    */
    ctx->button.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 40, 1, 4096));

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

    // --- Smart LED logic config ---
    /* Количенство светодиодов
    */
    ctx->led.num_of_led = get_option_int_val(slot_num, "numOfLed", "", 24, 1, 1024);

    /* Флаг инвертирует значение яркости 
    */
    ctx->led.inverse = get_option_flag_val(slot_num, "ledInverse");

    /* Состояние по умолчанию
    */
    ctx->led.state = get_option_int_val(slot_num, "defaultState", "", 0, 0, 1) ^ ctx->led.inverse;
      
    /* Максимальное значение яркости
    */
    ctx->led.maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 255);
    if(ctx->led.maxBright>255)ctx->led.maxBright=255;
    if(ctx->led.maxBright<0)ctx->led.maxBright=0;

    /* Минимальное значение яркости
    */
    ctx->led.minBright = get_option_int_val(slot_num, "minBright", "", 0, 0, 255);
    if(ctx->led.minBright<0)ctx->led.minBright=0;
    if(ctx->led.minBright>255)ctx->led.minBright=255;

    /* Частота обновления раз в секунду
    */
    ctx->led.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "hz", 1000/30, 1, 4096));
    

    /* Скорость изменения яркости в миллисекундах
    */
    ctx->led.fadeTime= get_option_int_val(slot_num, "fadeTime", "ms", 1000, 10, 10000);

    ctx->led.increment = (ctx->led.maxBright - ctx->led.minBright) * ctx->led.refreshPeriod / ctx->led.fadeTime;
    if (ctx->led.increment < 1) ctx->led.increment = 1;
    ESP_LOGD(TAG, "Calculated increment: %d for slot %d", ctx->led.increment, slot_num);

	
    /* Начальный цвет
    - по умолчанию \"0 0 255\" синий
    */
    if (get_option_color_val(&ctx->led.targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }

    /* Задаёт режим анимации 
    */ 
   //todo расписать
    if ((ctx->led.ledMode = get_option_enum_val(slot_num, "ledMode", "default", "flash", "rainbow", NULL)) < 0)
    {
        ESP_LOGE(TAG, "ledMode: unricognized value");
    }

    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
        /* Определяет топик для MQTT сообщений */
    	custom_topic = get_option_string_val(slot_num, "ledTopic", "/smartLed_0");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/smartLed_0")+3];
		sprintf(t_str, "%s/smartLed_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
	} 

    /* Числовое значение.
       задаёт текущее состояние светодиода (вкл/выкл)
    */
    stdcommand_register(&ctx->led.cmds, SMARTLED_default, NULL, PARAMT_int);

    /* Команда меняет текущее состояние светодиода на противоположное
    */
    stdcommand_register(&ctx->led.cmds, SMARTLED_toggleLedState, "toggleLedState", PARAMT_none);

    /* Команда задает цвет подсветки
    пример \"moduleBox/ledRing_0/setRGB:255 0 0\" установить красный цвет
    */
    stdcommand_register(&ctx->led.cmds, SMARTLED_setRGB, "setRGB", PARAMT_int, PARAMT_int, PARAMT_int);
    
    /* Установить новый режим анимации цветов
    
    */
    //todo расписать
    stdcommand_register_enum(&ctx->led.cmds, SMARTLED_setMode, "setMode", "default", "flash", "rainbow");
    
    /* Установить новое значение fadeTime, скорости анимации
    */
    stdcommand_register(&ctx->led.cmds, SMARTLED_setFadeTime, "setFadeTime", PARAMT_int);
}

static void setAllLed_color(uint8_t *pixel_array, RgbColor color, int16_t bright, uint16_t num_of_led){
    float fbright = (float)bright/255.0;
    uint8_t R = color.r*fbright;
    uint8_t G = color.g*fbright;
    uint8_t B = color.b*fbright;
    for(int i=0; i<num_of_led; i++){
        led_strip_set_pixel(pixel_array, i, R,G,B);
    }
}

void update_led_smart(PLEDCONFIG c, uint8_t *pixels, rmt_led_heap_t *rmt_heap, int slot_num, RgbColor *currentRGB, int16_t *currentBright, int16_t *targetBright)
{
    bool flag_ledUpdate = false;
    if(c->state==0){
        *targetBright = c->minBright;
        currentRGB->b=c->targetRGB.b;
        currentRGB->g=c->targetRGB.g;
        currentRGB->r=c->targetRGB.r;
        flag_ledUpdate = checkColorAndBright(currentRGB, &c->targetRGB, currentBright, targetBright, c->increment);
        setAllLed_color(pixels, *currentRGB, *currentBright, c->num_of_led);
    }else{
        if (c->ledMode==SMARTLED_MODE_default){ // MODE_DEFAULT
            *targetBright = c->maxBright;
            flag_ledUpdate = checkColorAndBright(currentRGB, &c->targetRGB, currentBright, targetBright, c->increment);
            setAllLed_color(pixels, *currentRGB, *currentBright, c->num_of_led);
        }else if(c->ledMode==SMARTLED_MODE_flash){ // MODE_FLASH
            if(*currentBright==c->minBright){
                *targetBright= c->maxBright;
            }else if(*currentBright==c->maxBright){
                *targetBright= c->minBright;
            }
            flag_ledUpdate = checkColorAndBright(currentRGB, &c->targetRGB, currentBright, targetBright, c->increment);
            setAllLed_color(pixels, *currentRGB, *currentBright, c->num_of_led);
        }else if(c->ledMode==SMARTLED_MODE_rainbow){ // MODE_RAINBOW
            *targetBright = c->maxBright;
            HsvColor hsv=RgbToHsv(c->targetRGB);
            hsv.h+=c->increment;
            c->targetRGB = HsvToRgb(hsv);
            flag_ledUpdate = checkColorAndBright(currentRGB, &c->targetRGB, currentBright, targetBright, c->increment);
            setAllLed_color(pixels, *currentRGB, *currentBright, c->num_of_led);
        }
    }

    if(flag_ledUpdate){
        rmt_createAndSend(rmt_heap, pixels, c->num_of_led * 3,  slot_num);
    }
}

void button_smartLed_task(void *arg)
{
    int slot_num = *(int*)arg;
    PMODULE_CONTEXT ctx = calloc(1, sizeof(MODULE_CONTEXT));
    setup_button_hw(slot_num, ctx);
    configure_button_smartLed(ctx, slot_num);

    if (rmt_semaphore == NULL) rmt_semaphore = xSemaphoreCreateCounting(1, 1);

    uint8_t pin_in = SLOTS_PIN_MAP[slot_num][0];
    uint8_t pin_out = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(10, sizeof(command_message_t));

    uint8_t pixels[ctx->led.num_of_led * 3];
    memset(pixels, 0, sizeof(pixels));

    rmt_led_heap_t rmt_heap = RMT_LED_HEAP_DEFAULT();
    rmt_heap.tx_chan_config.gpio_num = pin_out;
    rmt_new_led_strip_encoder(&rmt_heap.encoder_config, &rmt_heap.led_encoder);

    int prev_button_state = -1;
    int16_t currentBright = -1;
    int16_t targetBright = ctx->led.minBright;
    RgbColor currentRGB = {0, 0, 0};
    bool changeCompletedLogged = false;
    
    update_led_smart(&ctx->led, pixels, &rmt_heap, slot_num, &currentRGB, &currentBright, &targetBright);

    waitForWorkPermit(slot_num);
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx->led.cmds, &params, 0);
        if(cmd>=0){
            ESP_LOGD(TAG, "Slot_%d input cmd num:%d", slot_num, cmd);
        }
        switch (cmd) {
            case SMARTLED_default:
                ctx->led.state = params.p[0].i ^ ctx->led.inverse;
                if (ctx->led.state == 0) currentBright = targetBright - 1;
                break;
            case SMARTLED_setRGB:
                ctx->led.targetRGB.r = params.p[0].i;
                ctx->led.targetRGB.g = params.p[1].i;
                ctx->led.targetRGB.b = params.p[2].i;
                break;
            case SMARTLED_setMode:
                ctx->led.ledMode = params.enumResult;
                break;
            case SMARTLED_setFadeTime:
                ctx->led.fadeTime= params.p[0].i;
                ctx->led.increment = (ctx->led.maxBright - ctx->led.minBright) * ctx->led.refreshPeriod / ctx->led.fadeTime;
                if (ctx->led.increment < 1) ctx->led.increment = 1;
                break;
            case SMARTLED_toggleLedState:
                ctx->led.state = !ctx->led.state;
                if (ctx->led.state == 0) targetBright = ctx->led.minBright;
                if (ctx->led.state == 1) targetBright = ctx->led.maxBright;
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

        update_led_smart(&ctx->led, pixels, &rmt_heap, slot_num, &currentRGB, &currentBright, &targetBright);

        if (currentBright == targetBright && currentRGB.r == ctx->led.targetRGB.r && currentRGB.g == ctx->led.targetRGB.g && currentRGB.b == ctx->led.targetRGB.b) {
            if (!changeCompletedLogged) {
                ESP_LOGD(TAG, "Brightness and color change completed for slot %d", slot_num);
                changeCompletedLogged = true;
            }
        } else {
            changeCompletedLogged = false;
        }

        vTaskDelayUntil(&lastWakeTime, ctx->led.refreshPeriod);
    }
}

void start_button_smartLed_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_button_smartLed_%d", slot_num);
    xTaskCreate(button_smartLed_task, tmpString, 1024*8, &slot_num, configMAX_PRIORITIES-5, NULL);
}

const char * get_manifest_button_smartLed()
{
	return manifesto;
}