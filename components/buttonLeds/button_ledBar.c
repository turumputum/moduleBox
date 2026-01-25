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

#include <generated_files/gen_button_ledBar.h>

static const char *TAG = "BUTTON_LEDS";
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern SemaphoreHandle_t rmt_semaphore;
extern const uint8_t gamma_8[256];
// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

// --- button_ledBar ---
typedef enum
{
    LEDBAR_default = 0,
    LEDBAR_toggleLedState,
    LEDBAR_setRGB,
    LEDBAR_setPos,
} LEDBAR_CMD;

/* 
    Модуль кнопка со шкалой заполнения
*/
void configure_button_ledBar(PMODULE_CONTEXT ctx, int slot_num)
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
	ctx->button.stateReport = stdreport_register(RPTT_int, slot_num, "unit", nil, 0, 1);

	/* Рапортует при регистрации длинного нажатия
	*/
	ctx->button.longReport = stdreport_register(RPTT_int, slot_num, "unit", "longPress", 0, 1);

	/* Рапортует при регистрации двойного нажатия
	*/
	ctx->button.doubleReport = stdreport_register(RPTT_int, slot_num, "unit", "doubleClick", 0, 1);

    // --- LED Bar logic config ---
    /* Количенство светодиодов
    */
    ctx->led.num_of_led = get_option_int_val(slot_num, "numOfLed", "", 24, 1, 1024);

    /* Величина приращения
    */
    ctx->led.increment = get_option_int_val(slot_num, "increment", "", 255, 0, 255);
    if(ctx->led.increment<1)ctx->led.increment=1;
    if(ctx->led.increment>255)ctx->led.increment=255;

    /* Максимальное значение яркости
    */
    ctx->led.maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 4095);
    if(ctx->led.maxBright>255)ctx->led.maxBright=255;
    if(ctx->led.maxBright<0)ctx->led.maxBright=0;

    /* Минимальное значение яркости
    */
    ctx->led.minBright = get_option_int_val(slot_num, "minBright", "", 0, 0, 4095);
    if(ctx->led.minBright<0)ctx->led.minBright=0;
    if(ctx->led.minBright>255)ctx->led.minBright=255;

    /* Период обновления 
    */
    ctx->led.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 1000/30, 1, 4096));
    	
    /* Количество позиций
    */        
    ctx->led.numOfPos = get_option_int_val(slot_num, "numOfPos", "", ctx->led.num_of_led, 1, 4096);

    /* Состояние по умолчанию
    */
    ctx->led.state = get_option_int_val(slot_num, "defaultState", "", 0, 0, 1) ^ ctx->led.inverse;

    /* Инверсия направления эффекта
    */
    ctx->led.dir = ctx->led.inverse = get_option_flag_val(slot_num, "dirInverse") ? 1 : -1;

    /* Смещение эффекта
    */
    ctx->led.offset = get_option_int_val(slot_num, "offset", "", 0, 0, ctx->led.num_of_led);

    /* Начальный цвет
    */
    if (get_option_color_val(&ctx->led.targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }

    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
        /* Определяет топик для MQTT сообщений */
    	custom_topic = get_option_string_val(slot_num, "ledTopic", "/ledBar_0");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/ledBar_%d")+3];
		sprintf(t_str, "%s/ledBar_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
	} 
   
    /* задаёт текущее состояние светодиода (вкл/выкл)
    Числовое значение 0-1
    */
    stdcommand_register(&ctx->led.cmds, LEDBAR_default, NULL, PARAMT_int);

    /* Команда меняет текущее состояние светодиода на противоположное
    */
    stdcommand_register(&ctx->led.cmds, LEDBAR_toggleLedState, "toggleLedState", PARAMT_none);

    /* Команда задает цвет подсветки
    пример \"moduleBox/ledRing_0/setRGB:255 0 0\" установить красный цвет
    */
    stdcommand_register(&ctx->led.cmds, LEDBAR_setRGB, "setRGB", PARAMT_int, PARAMT_int, PARAMT_int);

    /* Команда задает положение светового эффекта
    пример \"moduleBox/ledBar_0/setPos:12\"
    */
    stdcommand_register(&ctx->led.cmds, LEDBAR_setPos, "setPos", PARAMT_int);
}

static uint8_t colorChek(uint8_t currentColor, uint8_t targetColor, uint8_t increment){
    if(currentColor<targetColor){
        if(targetColor-currentColor>increment) return currentColor+increment;
        else return targetColor;
    }else if(currentColor>targetColor){
        if(currentColor-targetColor>increment) return currentColor-increment;
        else return targetColor;
    }else return targetColor;
}

void update_led_bar(PLEDCONFIG c, uint8_t *pixels, uint8_t *current_bright_mass, uint8_t *target_bright_mass, rmt_led_heap_t *rmt_heap, int slot_num, RgbColor *currentRGB, float *currentPos, int *targetPos)
{
    bool flag_ledUpdate = false;
    float ledToPosRatio = (float)c->num_of_led/c->numOfPos;
    if(*targetPos!=*currentPos){
        *currentPos=*targetPos;
        float ledPos = (ledToPosRatio * (*currentPos));
        for(int i=0;i<c->num_of_led;i++){
            int curentLedPos = i + c->offset;
            if(curentLedPos>c->num_of_led-1) curentLedPos = curentLedPos - c->num_of_led;
            if((i == (int)ledPos)&&(i>0)){
                float ratio = ledPos - (int)ledPos;
                target_bright_mass[curentLedPos]=(int)(c->maxBright*ratio);
                if(target_bright_mass[curentLedPos]<c->minBright) target_bright_mass[curentLedPos]=c->minBright;
            }else if(i>(int)ledPos-1) target_bright_mass[curentLedPos]=c->minBright;
            else target_bright_mass[curentLedPos]=c->maxBright;
        }
    }
    if(memcmp(currentRGB, &c->targetRGB, sizeof(RgbColor))){
        currentRGB->r = colorChek(currentRGB->r, c->targetRGB.r, c->increment);
        currentRGB->g = colorChek(currentRGB->g, c->targetRGB.g, c->increment);
        currentRGB->b = colorChek(currentRGB->b, c->targetRGB.b, c->increment);
        flag_ledUpdate=true;
    }
    for(int i=0;i<c->num_of_led;i++){
        if(target_bright_mass[i]!=current_bright_mass[i]){
            flag_ledUpdate=true;
            if(target_bright_mass[i]>current_bright_mass[i]){
                if(i>0 && current_bright_mass[i-1]!=target_bright_mass[i-1]) break;
                if(abs(target_bright_mass[i]-current_bright_mass[i])<c->increment) current_bright_mass[i] = target_bright_mass[i];
                else current_bright_mass[i] = current_bright_mass[i] + c->increment;
                break;
            }else{
                if(i<c->num_of_led-1 && current_bright_mass[i+1]!=target_bright_mass[i+1]) break;
                if(abs(target_bright_mass[i]-current_bright_mass[i])<c->increment) current_bright_mass[i] = target_bright_mass[i];
                else current_bright_mass[i] = current_bright_mass[i] - c->increment;
                break;
            }
        }
    }
    if(flag_ledUpdate){
        for(int i=0;i<c->num_of_led;i++){
            int index = i;
            if(c->dir<0) index = c->num_of_led - i - 1;
            float  tmpBright = (float)current_bright_mass[i]/255;
            pixels[index*3] = gamma_8[(uint8_t)(currentRGB->r * tmpBright)];
            pixels[index*3+1] = gamma_8[(uint8_t)(currentRGB->g * tmpBright)];
            pixels[index*3+2] = gamma_8[(uint8_t)(currentRGB->b * tmpBright)];
        }
        rmt_createAndSend(rmt_heap, pixels, c->num_of_led * 3,  slot_num);
    }
}

void button_ledBar_task(void *arg)
{
    int slot_num = *(int*)arg;
    PMODULE_CONTEXT ctx = calloc(1, sizeof(MODULE_CONTEXT));
    setup_button_hw(slot_num, ctx);
    configure_button_ledBar(ctx, slot_num);

    if (rmt_semaphore == NULL) {
        rmt_semaphore = xSemaphoreCreateCounting(1, 1);
    }

    uint8_t pin_in = SLOTS_PIN_MAP[slot_num][0];
    uint8_t pin_out = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    uint8_t pixels[ctx->led.num_of_led * 3];
    uint8_t current_bright_mass[ctx->led.num_of_led];
    uint8_t target_bright_mass[ctx->led.num_of_led];
    memset(pixels, 0, sizeof(pixels));
    memset(current_bright_mass, 0, sizeof(current_bright_mass));
    memset(target_bright_mass, 0, sizeof(target_bright_mass));

    rmt_led_heap_t rmt_heap = RMT_LED_HEAP_DEFAULT();
    rmt_heap.tx_chan_config.gpio_num = pin_out;
    rmt_new_led_strip_encoder(&rmt_heap.encoder_config, &rmt_heap.led_encoder);

    int prev_button_state = -1;
    RgbColor currentRGB = {0, 0, 0};
    float currentPos = -1;
    int targetPos = 0;

    waitForWorkPermit(slot_num);
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {
        STDCOMMAND_PARAMS params = {0};
        switch (stdcommand_receive(&ctx->led.cmds, &params, 0)) {
            case LEDBAR_default:
                ctx->led.state = params.p[0].i ^ ctx->led.inverse;
                break;
            case LEDBAR_toggleLedState:
                ctx->led.state = !ctx->led.state;
                break;
            case LEDBAR_setRGB:
                ctx->led.targetRGB.r = params.p[0].i;
                ctx->led.targetRGB.g = params.p[1].i;
                ctx->led.targetRGB.b = params.p[2].i;
                break;
            case LEDBAR_setPos:
                targetPos += params.p[0].i;
                if(targetPos > ctx->led.numOfPos) targetPos = ctx->led.numOfPos;
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

        update_led_bar(&ctx->led, pixels, current_bright_mass, target_bright_mass, &rmt_heap, slot_num, &currentRGB, &currentPos, &targetPos);

        vTaskDelayUntil(&lastWakeTime, ctx->led.refreshPeriod);
    }
}

void start_button_ledBar_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_button_ledBar_%d", slot_num);
    xTaskCreate(button_ledBar_task, tmpString, 1024*4, &slot_num, configMAX_PRIORITIES-5, NULL);
}

const char * get_manifest_button_ledBar()
{
	return manifesto;
}
