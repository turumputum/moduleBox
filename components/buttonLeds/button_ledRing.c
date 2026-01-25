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

#include <generated_files/gen_button_ledRing.h>

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

// --- button_ledRing ---
typedef enum
{
    LEDRING_default = 0,
    LEDRING_toggleLedState,
    LEDRING_setRGB,
    LEDRING_setPos,
    LEDRING_setMode
} LEDRING_CMD;

typedef enum
{
    LEDRING_MODE_default = 0,
    LEDRING_MODE_run
} LEDRING_MODE;

/* 
    Модуль кнопка со светодиодным кольцом
*/
void configure_button_ledRing(PMODULE_CONTEXT ctx, int slot_num)
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
    при значениии 0 функция деактивирована
    по умолчанию 0
    */
    ctx->button.longPressTime 	= get_option_int_val(slot_num, "longPressTime", "ms", 0, 0, 10000);

    /* Длительность промежутка между нажатиями для регистрации двойного нажатия
    при значениии 0 функция деактивирована
    по умолчанию 0
    */
    ctx->button.doubleClickTime = get_option_int_val(slot_num, "doubleClickTime", "ms", 0, 0, 10000);

    /* Флаг задаёт фильтрацию совытий при активных
    */
    ctx->button.event_filter = get_option_flag_val(slot_num, "eventFilter");

    /* Частота обновления раз в секунду
    */
    ctx->button.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "hz", 40, 1, 100));

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

    // --- LED Ring logic config ---
    /* Количенство светодиодов
    */
    ctx->led.num_of_led = get_option_int_val(slot_num, "numOfLed", "", 24, 1, 1024);

    /* Величина приращения
    Скорость анимации
    */
    ctx->led.increment = get_option_int_val(slot_num, "increment", "", 255, 0, 255);
    if(ctx->led.increment<1)ctx->led.increment=1;
    if(ctx->led.increment>255)ctx->led.increment=255;

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


    ctx->led.refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 30, 1, 1024));

    /* Количество положений
    */        
    ctx->led.numOfPos = get_option_int_val(slot_num, "numOfPos", "", ctx->led.num_of_led, 1, 4096);

    /* Длинна эффекта
    */
    ctx->led.effectLen = get_option_int_val(slot_num, "effectLen", "", ctx->led.num_of_led / 4, 1, 4096);

    /* Инверсия направления эффекта
    */
    ctx->led.dir = get_option_flag_val(slot_num, "dirInverse") ? 1 : -1;
    ctx->led.inverse = get_option_flag_val(slot_num, "dirInverse") ? 1 : 0;

    /* Состояние по умолчанию
    */
    ctx->led.state = get_option_int_val(slot_num, "defaultState", "", 0, 0, 1) ^ ctx->led.inverse;

    /* Смещение эффекта
    */
    ctx->led.offset = get_option_int_val(slot_num, "offset", "", 0, 0, ctx->led.num_of_led);

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
   if ((ctx->led.ledMode = get_option_enum_val(slot_num, "ledMode", "default", "run", NULL)) < 0)
   {
       ESP_LOGE(TAG, "ledMode: unricognized value");
   }

    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
        /* Определяет топик для MQTT сообщений */
    	custom_topic = get_option_string_val(slot_num, "ledTopic", "/ledRing_0");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/ledRing_0")+3];
		sprintf(t_str, "%s/ledRing_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
	} 

    /* Задаёт текущее состояние светодиода (вкл/выкл)
    */
    stdcommand_register(&ctx->led.cmds, LEDRING_default, NULL, PARAMT_int);

    /* Команда меняет текущее состояние светодиода на противоположное
    */
    stdcommand_register(&ctx->led.cmds, LEDRING_toggleLedState, "toggleLedState", PARAMT_none);

    /* Команда задает цвет подсветки
    пример \"moduleBox/ledRing_0/setRGB:255 0 0\" установить красный цвет
    */
    stdcommand_register(&ctx->led.cmds, LEDRING_setRGB, "setRGB", PARAMT_int, PARAMT_int, PARAMT_int);
    
    /* Команда задает положение светового эффекта, используется при подсветке спиннера
    пример \"moduleBox/ledRing_0/setPos:12\"
    */
    stdcommand_register(&ctx->led.cmds, LEDRING_setPos, "setPos", PARAMT_int);

    /* Установить новый режим анимации цветов
    
    */
    //todo расписать
    stdcommand_register_enum(&ctx->led.cmds, LEDRING_setMode, "setMode", "default", "run");
    
}

static void ledUpdate(uint8_t *currentMass, uint8_t *targetMass, uint16_t size, uint8_t increment, rmt_led_heap_t *rmt_heap, uint8_t slot_num) {
    uint16_t sum = 0;
    for(int i=0; i<size; i++){
        if(currentMass[i] != targetMass[i]){
            sum ++;
            if(currentMass[i] < targetMass[i]){
                int16_t temp = currentMass[i]+ increment;
                if(temp >= targetMass[i]){
                    currentMass[i] = targetMass[i];
                }else{
                    currentMass[i] += increment;
                }
            }else{
                int16_t temp = currentMass[i] - increment;
                if(temp <= targetMass[i]){
                    currentMass[i] = targetMass[i];
                }else{
                    currentMass[i] -= increment;
                }
            }
        }
    }
    if(sum != 0){
        //ESP_LOG_BUFFER_HEX(TAG, currentMass, size);
        rmt_createAndSend(rmt_heap, currentMass, size, slot_num);
    }
}

static void calcRingBrightness(uint8_t *pixelMass, RgbColor color, uint16_t num_of_led, float centrPos, uint16_t effectLen, uint16_t numOfPos, int16_t maxBright, int16_t minBright) {
    float positionScale = (float)num_of_led / numOfPos;
    float scaledCentrPos = (centrPos * positionScale);
    float fMaxB = (float)maxBright/255;
    float fMinB = (float)minBright/255;
    uint16_t halfEffect = effectLen/2;
    for(int i = 0; i < num_of_led; i++) {
        float distance = i - scaledCentrPos;
        if(distance > num_of_led/2) distance -= num_of_led;
        if(distance < -num_of_led/2) distance += num_of_led;
        distance = fabs(distance);
        if(distance <= halfEffect) {
            float ratio =(1- distance / halfEffect) * (fMaxB - fMinB) + fMinB;
            pixelMass[i*3] = gamma_8[(uint8_t)(color.g * ratio)];
            pixelMass[i*3+1] = gamma_8[(uint8_t)(color.r * ratio)];
            pixelMass[i*3+2] = gamma_8[(uint8_t)(color.b * ratio)];
        } else {
            pixelMass[i*3] = gamma_8[(uint8_t)(color.g * fMinB)];
            pixelMass[i*3+1] = gamma_8[(uint8_t)(color.r * fMinB)];
            pixelMass[i*3+2] = gamma_8[(uint8_t)(color.b * fMinB)];
        }
    }
}

void update_led_ring(PLEDCONFIG c, uint8_t *current_pixels, uint8_t *target_pixels, rmt_led_heap_t *rmt_heap, int slot_num, float *currentPos, float *targetPos, uint8_t *prevState)
{
    float fIncrement = (float)c->increment/255;
    if(c->state==1){
        if(c->state!=*prevState){
            //ESP_LOGD(TAG, "LED Ring Slot %d: state changed to ON", slot_num);
            *prevState=c->state;
            float fMaxB = (float)c->maxBright/255;
            for(int i = 0; i < c->num_of_led; i++) {
                target_pixels[i*3] = gamma_8[(uint8_t)(c->targetRGB.g * fMaxB)];
                target_pixels[i*3+1] = gamma_8[(uint8_t)(c->targetRGB.r * fMaxB)];
                target_pixels[i*3+2] = gamma_8[(uint8_t)(c->targetRGB.b * fMaxB)]; 
            }
        }   
    }else{
        if(c->state!=*prevState){
            *prevState=c->state;
            *currentPos = *targetPos-fIncrement;
            ESP_LOGD(TAG, "LED Ring Slot %d: state changed to OFF curPos:%ld tarPos:%ld", slot_num, (int32_t)*currentPos, (int32_t)*targetPos);
        }
        if(c->ledMode==LEDRING_MODE_run){ // MODE_RUN
            *targetPos += fIncrement*c->dir;
            if(*targetPos<0){
                *targetPos=*targetPos+(c->numOfPos);
            }
            while(*targetPos > (c->numOfPos-1)){
                *targetPos=*targetPos-(c->numOfPos);  
            }
        }
        if(*currentPos!=*targetPos){
            if(fabs(*currentPos-*targetPos)<fIncrement){
                *currentPos = *targetPos;
            }else{
                if(fabs(*currentPos-*targetPos)<c->numOfPos/2){
                    *currentPos = (*currentPos>*targetPos) ? *currentPos-fIncrement : *currentPos+fIncrement;
                }else{
                    *currentPos = (*currentPos>*targetPos) ? *currentPos+fIncrement : *currentPos-fIncrement;
                }
                if(*currentPos<0){
                    *currentPos=*currentPos+(c->numOfPos);
                }else if(*currentPos>c->numOfPos-1){
                    *currentPos=*currentPos-(c->numOfPos);
                }
            }
            calcRingBrightness(target_pixels, c->targetRGB, c->num_of_led, *currentPos, c->effectLen, c->numOfPos, c->maxBright, c->minBright);
        }
    }
    ledUpdate(current_pixels, target_pixels, c->num_of_led*3, c->increment, rmt_heap, slot_num);
}

void button_ledRing_task(void *arg)
{
    int slot_num = *(int*)arg;
    PMODULE_CONTEXT ctx = calloc(1, sizeof(MODULE_CONTEXT));
    setup_button_hw(slot_num, ctx);
    configure_button_ledRing(ctx, slot_num);

    if (rmt_semaphore == NULL) rmt_semaphore = xSemaphoreCreateCounting(1, 1);

    uint8_t pin_in = SLOTS_PIN_MAP[slot_num][0];
    uint8_t pin_out = SLOTS_PIN_MAP[slot_num][1];

    me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    uint8_t current_pixels[ctx->led.num_of_led * 3];
    uint8_t target_pixels[ctx->led.num_of_led * 3];
    memset(current_pixels, 0, sizeof(current_pixels));
    memset(target_pixels, 0, sizeof(target_pixels));

    rmt_led_heap_t rmt_heap = RMT_LED_HEAP_DEFAULT();
    rmt_heap.tx_chan_config.gpio_num = pin_out;
    rmt_new_led_strip_encoder(&rmt_heap.encoder_config, &rmt_heap.led_encoder);

    int prev_button_state = -1;
    float currentPos = ctx->led.numOfPos - 1 + ctx->led.offset;
    float targetPos = ctx->led.offset;
    uint8_t prevState = 255;

    waitForWorkPermit(slot_num);
    TickType_t lastWakeTime = xTaskGetTickCount();

    while (1) {
        STDCOMMAND_PARAMS params = {0};
        int cmd = stdcommand_receive(&ctx->led.cmds, &params, 0);
        if(cmd>=0)ESP_LOGD(TAG, "Button LED Ring Slot %d: received command %d param:%ld", slot_num, cmd,  params.p[0].i);

        switch (cmd) {
            case LEDRING_default:
                ctx->led.state = ((params.p[0].i != 0) ? 1 : 0) ^ ctx->led.inverse;
                ESP_LOGD(TAG, "LED Ring Slot %d: set state to %d", slot_num, ctx->led.state);
                break;
            case LEDRING_setRGB:
                ctx->led.targetRGB.r = params.p[0].i;
                ctx->led.targetRGB.g = params.p[1].i;
                ctx->led.targetRGB.b = params.p[2].i;
                break;
            case LEDRING_setPos:
                if(ctx->led.dir==1) targetPos = params.p[0].i + ctx->led.offset;
                else targetPos = ctx->led.numOfPos - 1 - params.p[0].i + ctx->led.offset;
                while(targetPos < 0) targetPos += ctx->led.numOfPos;
                while(targetPos > ctx->led.numOfPos - 1) targetPos -= ctx->led.numOfPos;
                break;
            case LEDRING_toggleLedState:
                ctx->led.state = !ctx->led.state;
                break;
            case LEDRING_setMode:
                ctx->led.ledMode = params.p[0].i;
                ESP_LOGD(TAG, "LED Ring Slot %d: set mode to %d", slot_num, ctx->led.ledMode);
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

        update_led_ring(&ctx->led, current_pixels, target_pixels, &rmt_heap, slot_num, &currentPos, &targetPos, &prevState);

        vTaskDelayUntil(&lastWakeTime, ctx->led.refreshPeriod);
    }
}

void start_button_ledRing_task(int slot_num) {
    char tmpString[60];
    sprintf(tmpString, "task_button_ledRing_%d", slot_num);
    xTaskCreate(button_ledRing_task, tmpString, 1024*4, &slot_num, configMAX_PRIORITIES-5, NULL);
}

const char * get_manifest_button_ledRing()
{
	return manifesto;
}