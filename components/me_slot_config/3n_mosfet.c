#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gptimer.h"
#include "driver/ledc.h"
#include "esp_log.h"

#include "executor.h"
#include "3n_mosfet.h"
#include "me_slot_config.h"
#include "stateConfig.h"

#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include <stdcommand.h>

#include "rgbHsv.h"

#include <generated_files/gen_3n_mosfet.h>

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_MOSFETCONFIG
{
    int16_t                 increment;
    uint8_t                 inverse;
    int16_t                 max_bright;
    int16_t                 min_bright;
    uint16_t                refreshPeriod;
    RgbColor                targetRGB;
    uint8_t                 ledMode;
    uint8_t                 state;
    STDCOMMANDS             cmds;

} MOSFETCONFIG, * PMOSFETCONFIG; 

typedef enum
{
    MYCMD_default = 0,
    MYCMD_setRGB,
    MYCMD_setMode,
    MYCMD_setIncrement,
    MYCMD_setMaxBright,
    MYCMD_setMinBright
} MYCMD;

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------


extern uint8_t SLOTS_PIN_MAP[10][4];

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "3n_MOSFET";

extern configuration me_config;
extern stateStruct me_state;

extern const uint8_t gamma_8[256];

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------



void set_pwm_channels(ledc_channel_config_t ch_r, ledc_channel_config_t ch_g, ledc_channel_config_t ch_b, RgbColor color, uint8_t bright){
	//ESP_LOGI(TAG, "Set PWM channels: %d %d %d", color.r,color.g,color.b);
	float bbright = (float)bright/255.0;
    uint8_t R = (uint8_t)(color.r*bbright);
    uint8_t G = (uint8_t)(color.g*bbright);
    uint8_t B = (uint8_t)(color.b*bbright);
    //ESP_LOGI(TAG, "Set PWM channels: %d %d %d  ::  bright:%f", R,G,B, bbright);
	ledc_set_duty(LEDC_MODE, ch_r.channel, gamma_8[R]);
	ledc_set_duty(LEDC_MODE, ch_g.channel, gamma_8[G]);
	ledc_set_duty(LEDC_MODE, ch_b.channel, gamma_8[B]);
	ledc_update_duty(LEDC_MODE, ch_r.channel);
	ledc_update_duty(LEDC_MODE, ch_g.channel);
	ledc_update_duty(LEDC_MODE, ch_b.channel);
}
/*
    Модуль управляет RGB-лентой
*/
void configure_pwmRGB(PMOSFETCONFIG c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* Величина приращения значения свечения
    */
    c->increment = get_option_int_val(slot_num, "increment", "", 255, 1, 4096);
    if(c->increment>255)c->increment=255;
    if(c->increment<1)c->increment=1;
    ESP_LOGD(TAG, "Set increment:%d for slot:%d", c->increment, slot_num);

    /* Инверсия значений
    */
    c->inverse = get_option_flag_val(slot_num, "inverse");
    ESP_LOGD(TAG, "Set inverse:%d for slot:%d", c->inverse, slot_num);

    /* Максимальная яркость
    */
    c->max_bright = get_option_int_val(slot_num, "maxBright", "", 255, 1, 4096);
    if(c->max_bright>255)c->max_bright=255;
    if(c->max_bright<0)c->max_bright=0;
    ESP_LOGD(TAG, "Set max_bright:%d for slot:%d",c->max_bright, slot_num);

    /* Минимальная яркость
    */
    c->min_bright = get_option_int_val(slot_num, "minBright", "", 0, 1, 4096);
    if(c->min_bright>255)c->min_bright=255;
    if(c->min_bright<0)c->min_bright=0;
    ESP_LOGD(TAG, "Set min_bright:%d for slot:%d",c->min_bright, slot_num);

    /* Период обновления
    */
    c->refreshPeriod = 1000/get_option_int_val(slot_num, "refreshRate", "", 40, 1, 4096);
    ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",c->refreshPeriod, slot_num);
    	
    /* Начальный цвет
    */
    if (get_option_color_val(&c->targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }
    else
        ESP_LOGD(TAG, "Set color:%d %d %d for slot:%d", c->targetRGB.r, c->targetRGB.g, c->targetRGB.b, slot_num);

    /* Задаёт режим анимации */
    if ((c->ledMode = get_option_enum_val(slot_num, "ledMode", "default", "flash", "glitch", "swiper", "rainbow", "run", NULL)) < 0)
    {
        ESP_LOGE(TAG, "ledMode: unricognized value");
        c->ledMode = MODE_DEFAULT;
    }

    /* Состояние по умолчанию
    */
    c->state = get_option_int_val(slot_num, "defaultState", "", 0, 1, 4096);
    ESP_LOGD(TAG, "Set def_state:%d for slot:%d", c->state, slot_num);

	if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
        /* Определяет топик для MQTT сообщений */
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/pwmRGB_0")+3];
		sprintf(t_str, "%s/pwmRGB_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    /* Числовое значение.
       задаёт текущее состояние светодиода (вкл/выкл)
    */
    stdcommand_register(&c->cmds, MYCMD_default, NULL, PARAMT_int);

    /* Установить новый целевой цвет. 
       Цвет задаётся десятичными значениями R G B через пробел
    */
    stdcommand_register(&c->cmds, MYCMD_setRGB, "setRGB", PARAMT_int, PARAMT_int, PARAMT_int);

    /* Установить новый режим анимации цветов
    */
    stdcommand_register_enum(&c->cmds, MYCMD_setMode, "setMode", "default", "flash", "glitch", "swiper", "rainbow", "run");

    /* Установить новое значение приращения
    */
    stdcommand_register(&c->cmds, MYCMD_setIncrement, "setIncrement", PARAMT_int);

    /* Установить максимальное значение яркости
    */
    stdcommand_register(&c->cmds, MYCMD_setMaxBright, "setMaxBright", PARAMT_int);

    /* Установить минимальное значение яркости
    */
    stdcommand_register(&c->cmds, MYCMD_setMinBright, "setMinBright", PARAMT_int);
}

void rgb_ledc_task(void *arg){
    PMOSFETCONFIG c = calloc(1, sizeof(MOSFETCONFIG));
    uint32_t startTick = xTaskGetTickCount();
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
    STDCOMMAND_PARAMS       params = { 0 };

	me_state.command_queue[slot_num] = xQueueCreate(50, sizeof(command_message_t));

    configure_pwmRGB(&c, slot_num);

	//if(chennelCounter<2){
		ledc_timer_config_t ledc_timer = {
			.speed_mode = LEDC_MODE,
			.timer_num = LEDC_TIMER,
			.duty_resolution = LEDC_DUTY_RES,
			.freq_hz = LEDC_FREQUENCY,  // Set output frequency at 5 kHz
			.clk_cfg = LEDC_AUTO_CLK };
		ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));
		ESP_LOGD(TAG, "LEDC timer inited");
	//}

	if((LEDC_CHANNEL_MAX - me_state.ledc_chennelCounter)<3){
		ESP_LOGE(TAG, "LEDC channel has ended");
		goto EXIT;
	}

	ledc_channel_config_t ledc_ch_R = {
		.speed_mode = LEDC_MODE,
		.channel = me_state.ledc_chennelCounter++,
		.timer_sel = LEDC_TIMER,
		.intr_type = LEDC_INTR_DISABLE,
		.gpio_num = SLOTS_PIN_MAP[slot_num][0],
		.duty = 0, // Set duty to 0%
		.hpoint = 0 };

	ledc_channel_config_t ledc_ch_G = {
		.speed_mode = LEDC_MODE,
		.channel = me_state.ledc_chennelCounter++,
		.timer_sel = LEDC_TIMER,
		.intr_type = LEDC_INTR_DISABLE,
		.gpio_num = SLOTS_PIN_MAP[slot_num][1],
		.duty = 0, // Set duty to 0%
		.hpoint = 0 };

	ledc_channel_config_t ledc_ch_B = {
		.speed_mode = LEDC_MODE,
		.channel = me_state.ledc_chennelCounter++,
		.timer_sel = LEDC_TIMER,
		.intr_type = LEDC_INTR_DISABLE,
		.gpio_num = SLOTS_PIN_MAP[slot_num][2],
		.duty = 0, // Set duty to 0%
		.hpoint = 0 };
	ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch_R));
	ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch_G));
	ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch_B));
	ESP_LOGD(TAG, "LEDC channel counter:%d", me_state.ledc_chennelCounter);

	int16_t currentBright=0;
    int16_t targetBright=abs(255*c->inverse-c->min_bright);
    RgbColor currentRGB={
        .r=0,
        .g=0,
        .b=0
    };
	
    uint8_t aniSwitch=0;
    TickType_t lastWakeTime = xTaskGetTickCount(); 

    waitForWorkPermit(slot_num);

    while (1) {

        switch (stdcommand_receive(&c->cmds, &params, 0))
        {
            case -1: // none
                break;


            case MYCMD_default:
                c->state = params.p[0].data;
                break;

            case MYCMD_setRGB:
                c->targetRGB.r = params.p[0].data;
                c->targetRGB.g = params.p[1].data;
                c->targetRGB.b = params.p[2].data;

                ESP_LOGD(TAG, "Slot:%d target RGB: %d %d %d", slot_num, c->targetRGB.r, c->targetRGB.g, c->targetRGB.b); 
                break;

            case MYCMD_setMode:
                c->ledMode = params.enumResult;
                break;

            case MYCMD_setIncrement:
                c->increment = params.p[0].data;
                ESP_LOGD(TAG, "Set fade increment:%d", c->increment);
                break;

            case MYCMD_setMaxBright:
                c->max_bright = params.p[0].data;
                break;

            case MYCMD_setMinBright:
                c->min_bright = params.p[0].data;
                break;

            default:
                break;                
        }

        if(c->state==0){
            targetBright =abs(255*c->inverse-c->min_bright); 
            checkColorAndBright(&currentRGB, &c->targetRGB, &currentBright, &targetBright, c->increment);
			set_pwm_channels(ledc_ch_R, ledc_ch_G,ledc_ch_B, currentRGB,currentBright);
        }else{
            if (c->ledMode==MODE_DEFAULT){
                targetBright = abs(255*c->inverse-c->max_bright); 
                checkColorAndBright(&currentRGB, &c->targetRGB, &currentBright, &targetBright, c->increment);
                set_pwm_channels(ledc_ch_R, ledc_ch_G,ledc_ch_B, currentRGB ,currentBright);
                //ESP_LOGD(TAG, "pwmRGB currentBright:%f targetBright:%f", currentBright, targetBright); 
            }else if(c->ledMode==MODE_FLASH){
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d", currentBright, targetBright); 
                if(currentBright==c->min_bright){
                    targetBright=abs(255*c->inverse-c->max_bright);
                    //ESP_LOGD(TAG, "Flash min bright:%d targetBright:%d", currentBright, targetBright); 
                }else if(currentBright==c->max_bright){
                    targetBright=fabs(255*c->inverse-c->min_bright);
                    //ESP_LOGD(TAG, "Flash max bright:%d targetBright:%d", currentBright, targetBright); 
                }
                checkColorAndBright(&currentRGB, &c->targetRGB, &currentBright, &targetBright, c->increment);
                set_pwm_channels(ledc_ch_R, ledc_ch_G,ledc_ch_B, currentRGB, currentBright);
            }else if(c->ledMode==MODE_RAINBOW){
                targetBright = c->max_bright;
                HsvColor hsv=RgbToHsv(c->targetRGB);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d H:%d S:%d V:%d",currentBright, targetBright, hsv.h, hsv.s, hsv.v);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d R:%d G:%d B:%d", currentBright, targetBright, currentRGB.r, currentRGB.g, currentRGB.b);
                //ESP_LOGD(TAG, "hsv before:%d %d %d", hsv.h, hsv.s, hsv.v);
                hsv.h+=c->increment;
                //hsv.s = 255;
                //hsv.v = (uint8_t)(max_bright*255);
                //ESP_LOGD(TAG, "hsv after:%d %d %d", hsv.h, hsv.s, hsv.v);
                c->targetRGB = HsvToRgb(hsv);
                //ESP_LOGD(TAG, "Flash currentBright:%f targetBright:%f R:%d G:%d B:%d", currentBright, targetBright, targetRGB.r, targetRGB.g, targetRGB.b);
                checkColorAndBright(&currentRGB, &c->targetRGB, &currentBright, &targetBright, c->increment);
                set_pwm_channels(ledc_ch_R, ledc_ch_G,ledc_ch_B, currentRGB, currentBright);
            }
        }
        vTaskDelayUntil(&lastWakeTime, c->refreshPeriod);
    }

	EXIT:
    vTaskDelete(NULL);
}

void init_3n_mosfet(int slot_num) {
	uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreate(rgb_ledc_task, "smartLed_task", 1024*4, &slot_num,12, NULL);

	ESP_LOGD(TAG,"pwmRGB_led task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

const char * get_manifest_3n_mosfet()
{
	return manifesto;
}


// void setRGB(int slot_num, char* payload){
// 	ESP_LOGD(TAG, "Set RGB for slot:%d val:%s",slot_num, payload);
// 	char *rest;
// 	char *tok;
// 	if(strstr(payload, " ")!=NULL){
// 		tok = strtok_r(payload, " ", &rest);
// 		R = atoi(tok);
// 		tok = strtok_r(NULL, " ", &rest);
// 		G = atoi(tok);
// 		B = atoi(rest);

// 		if (R > 255) {
// 			do {
// 				R -= 255;
// 			} while (R > 255);
// 		}
// 		if (G > 255) {
// 			do {
// 				G -= 255;
// 			} while (G > 255);
// 		}
// 		if (B > 255) {
// 			do {
// 				B -= 255;
// 			} while (B > 255);
// 		}
// 		set_pwm_channels(slot_num, R, G, B);
// 	}

// 	channelDebug();
// }

// void glitch_task(){
// 	flag_glitch_task=1;
// 	int state=0;
// 	int delay=rand()%(glitchVal*2) +10;
// 	while(1){
// 		if(state==0){
// 			set_pwm_channels(0, 0, 0, 0);
// 		}else{
// 			set_pwm_channels(0, R, G, B);
// 		}
// 		delay=rand()%glitchVal +glitchVal;
// 		state=!state;
// 		vTaskDelay(pdMS_TO_TICKS(delay));

// 		if(flag_glitch_task==0){
// 			vTaskDelete(NULL);
// 		}
// 	}
// }

// void setGlitch(int slot_num, char* payload){
// 	int val = atoi(payload);
// 	if(val==0){
// 		flag_glitch_task=0;
// 	}else if(flag_glitch_task!=1){
// 		xTaskCreate(glitch_task, "glitch", 1024 * 2, NULL, configMAX_PRIORITIES - 8, NULL);
// 	}
// 	glitchVal=val;
// }


//IRAM_ATTR
// static bool IRAM_ATTR pwm_ISR(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void* user_data){
    
// 	BaseType_t high_task_awoken = pdFALSE;

// 	pwm_ch_t pwm_ch_data = *((pwm_ch_t*)user_data);
//     // stop timer immediately
//     //gptimer_stop(timer);

// 	int duty = pwm_ch_data.duty;
// 	int16_t alarmCount=1;
// 	if(state==1){
// 		//gpio_set_level(4, 1);
// 		GPIO.out_w1ts = ((uint32_t)1 << 4);
// 		alarmCount = duty;
// 	}else{
// 		//gpio_set_level(4, 0);
// 		GPIO.out_w1tc = ((uint32_t)1 << 4);
// 		alarmCount = 255 - duty;
// 	}
// 	if(alarmCount>254)alarmCount=254;
// 	if(alarmCount<1)alarmCount=1;

// 	gptimer_alarm_config_t alarm_config = {
//         .alarm_count =edata->alarm_value+alarmCount, // period = 1s
//     };
// 	gptimer_set_alarm_action(timer, &alarm_config);
// 	//gptimer_set_raw_count(timer, 0);
// 	//gptimer_start(timer);

// 	//ESP_LOGD(TAG, "tick");

// 	state =!state;

// 	return high_task_awoken == pdTRUE;
// }

// void start_pwm_ch(void){

// 	esp_rom_gpio_pad_select_gpio(4);
// 	gpio_set_direction(4, GPIO_MODE_OUTPUT);

// 	gptimer_config_t timer_config = {
//         .clk_src = GPTIMER_CLK_SRC_DEFAULT,
//         .direction = GPTIMER_COUNT_UP,
//         .resolution_hz = 125000,//125000, //125khz 1tick = 8us //1000000, // 1MHz, 1 tick=1us
//     };
// 	gptimer_new_timer(&timer_config, &timer);
	
// 	gptimer_event_callbacks_t cbs = {
//         .on_alarm = pwm_ISR,
//     };

// 	pwm_ch_test.duty = 250;

// 	gptimer_register_event_callbacks(timer, &cbs, &pwm_ch_test);
// 	gptimer_enable(timer);

// 	gptimer_alarm_config_t alarm_config = {
// 		.reload_count = 0,
//         .alarm_count = pwm_ch_test.duty, // 
// 		.flags.auto_reload_on_alarm = 1,
//     };
// 	gptimer_set_alarm_action(timer, &alarm_config);
// 	gptimer_start(timer);


// 	gpio_set_level(4, 1);

// 	ESP_LOGD(TAG, "test timer started");
// }

