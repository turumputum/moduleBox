#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

#include "rgbHsv.h"

extern uint8_t SLOTS_PIN_MAP[10][4];

uint8_t chennelCounter=0;

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
//#define LEDC_OUTPUT_IO          (10) // Define the output GPIO
//#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_8_BIT // Set duty resolution to 13 bits
#define LEDC_FREQUENCY          (5000) // Frequency in Hertz. Set frequency at 5 kHz

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "3n_MOSFET";

extern configuration me_config;
extern stateStruct me_state;

extern const uint8_t gamma_8[256];

void set_pwm_channels(ledc_channel_config_t ch_r, ledc_channel_config_t ch_g, ledc_channel_config_t ch_b, RgbColor color, int bright){
	//ESP_LOGI(TAG, "Set PWM channels: %d %d %d", color.r,color.g,color.b);
	float bbright = (float)bright/255;
	uint8_t R = color.r*bbright;
    uint8_t G = color.g*bbright;
    uint8_t B = color.b*bbright;
	ledc_set_duty(LEDC_MODE, ch_r.channel, gamma_8[R]);
	ledc_set_duty(LEDC_MODE, ch_g.channel, gamma_8[G]);
	ledc_set_duty(LEDC_MODE, ch_b.channel, gamma_8[B]);
	ledc_update_duty(LEDC_MODE, ch_r.channel);
	ledc_update_duty(LEDC_MODE, ch_g.channel);
	ledc_update_duty(LEDC_MODE, ch_b.channel);
}

void rgb_ledc_task(void *arg){
    uint32_t startTick = xTaskGetTickCount();
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];

	me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));
    
    uint16_t fade_increment = 255;
    if (strstr(me_config.slot_options[slot_num], "fade_increment") != NULL) {
		fade_increment = get_option_int_val(slot_num, "fade_increment");
		ESP_LOGD(TAG, "Set fade_increment:%d for slot:%d",fade_increment, slot_num);
	}

    uint16_t max_bright = 255;
    if (strstr(me_config.slot_options[slot_num], "max_bright") != NULL) {
		max_bright = get_option_int_val(slot_num, "max_bright");
        if(max_bright>255)max_bright=255;
		ESP_LOGD(TAG, "Set max_bright:%d for slot:%d",max_bright, slot_num);
	}

    uint16_t min_bright = 0;
    if (strstr(me_config.slot_options[slot_num], "min_bright") != NULL) {
		min_bright = get_option_int_val(slot_num, "min_bright");
        if(min_bright>255)min_bright=255;
		ESP_LOGD(TAG, "Set min_bright:%d for slot:%d",min_bright, slot_num);
	}

    uint16_t refreshRate_ms = 30;
    if (strstr(me_config.slot_options[slot_num], "refreshRate_ms") != NULL) {
		refreshRate_ms = get_option_int_val(slot_num, "refreshRate_ms");
		ESP_LOGD(TAG, "Set refreshRate_ms:%d for slot:%d",refreshRate_ms, slot_num);
	}
    	
    RgbColor targetRGB={
        .r=0,
        .g=0,
        .b=250
    };
    HsvColor HSV;
    if (strstr(me_config.slot_options[slot_num], "RGB_color") != NULL) {
        char strDup[strlen(me_config.slot_options[slot_num])];
        strcpy(strDup, me_config.slot_options[slot_num]);
    	char *tmp;
		tmp = strstr(strDup, "RGB_color")+strlen("RGB_color")+1;
		ESP_LOGD(TAG, "color:%s", tmp);
        char *rest;
        targetRGB.r = atoi(strtok_r(tmp," ",&rest));
        targetRGB.g = atoi(strtok_r(rest," ",&rest));
        targetRGB.b = atoi(strtok_r(rest," ",&rest));
		HSV = RgbToHsv(targetRGB);
    }
    ESP_LOGD(TAG, "Set color:%d %d %d for slot:%d", targetRGB.r, targetRGB.g, targetRGB.b, slot_num);

    uint8_t ledMode = DEFAULT;
    if (strstr(me_config.slot_options[slot_num], "ledMode") != NULL) {
        char* tmp=NULL;
    	tmp = get_option_string_val(slot_num, "ledMode");
        ledMode = modeToEnum(tmp);
		ESP_LOGD(TAG, "Set mode:%d for slot:%d", ledMode, slot_num);
    }

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

	if(chennelCounter>4){
		ESP_LOGE(TAG, "LEDC channel has ended");
		goto EXIT;
	}

	ledc_channel_config_t ledc_ch_R = {
		.speed_mode = LEDC_MODE,
		.channel = chennelCounter++,
		.timer_sel = LEDC_TIMER,
		.intr_type = LEDC_INTR_DISABLE,
		.gpio_num = SLOTS_PIN_MAP[slot_num][0],
		.duty = 0, // Set duty to 0%
		.hpoint = 0 };

	ledc_channel_config_t ledc_ch_G = {
		.speed_mode = LEDC_MODE,
		.channel = chennelCounter++,
		.timer_sel = LEDC_TIMER,
		.intr_type = LEDC_INTR_DISABLE,
		.gpio_num = SLOTS_PIN_MAP[slot_num][1],
		.duty = 0, // Set duty to 0%
		.hpoint = 0 };

	ledc_channel_config_t ledc_ch_B = {
		.speed_mode = LEDC_MODE,
		.channel = chennelCounter++,
		.timer_sel = LEDC_TIMER,
		.intr_type = LEDC_INTR_DISABLE,
		.gpio_num = SLOTS_PIN_MAP[slot_num][2],
		.duty = 0, // Set duty to 0%
		.hpoint = 0 };
	ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch_R));
	ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch_G));
	ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch_B));
	ESP_LOGD(TAG, "LEDC channel counter:%d", chennelCounter);

	if (strstr(me_config.slot_options[slot_num], "pwmRGBled_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "pwmRGBled_topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.device_name)+strlen("/pwmRGBled_0")+3];
		sprintf(t_str, "%s/pwmRGBled_%d",me_config.device_name, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 


	uint16_t currentBright=0;
    uint16_t targetBright=min_bright;
    RgbColor currentRGB={
        .r=0,
        .g=0,
        .b=0
    };
	uint8_t state = 0;
    uint8_t aniSwitch=0;

    while (1) {
        startTick = xTaskGetTickCount();

        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            if(strlen(cmd)==strlen(me_state.action_topic_list[slot_num])){
                state = atoi(payload);
                ESP_LOGD(TAG, "Change state to:%d", state);
            }else{
                cmd = cmd + strlen(me_state.action_topic_list[slot_num]);
                if(strstr(cmd, "setRGB")!=NULL){
                    parseRGB(&targetRGB, payload);
                }else if(strstr(cmd, "setMode")!=NULL){
                    ledMode = modeToEnum(payload);
                }else if(strstr(cmd, "setFadeIncrement")!=NULL){
                    fade_increment = atoi(payload);
                    ESP_LOGD(TAG, "Set fade increment:%d", fade_increment);
                }
            }
        }

        if(state==0){
            targetBright = min_bright; 
            checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, fade_increment);
			set_pwm_channels(ledc_ch_R, ledc_ch_G,ledc_ch_B, currentRGB,currentBright);
        }else{
            if (ledMode==DEFAULT){
                targetBright = max_bright; 
                checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, fade_increment);
                set_pwm_channels(ledc_ch_R, ledc_ch_G,ledc_ch_B, currentRGB ,currentBright);
            }else if(ledMode==FLASH){
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d", currentBright, targetBright); 
                if(currentBright==min_bright){
                    targetBright=max_bright;
                    //ESP_LOGD(TAG, "Flash min bright:%d targetBright:%d", currentBright, targetBright); 
                }else if(currentBright==max_bright){
                    targetBright=min_bright;
                    //ESP_LOGD(TAG, "Flash max bright:%d targetBright:%d", currentBright, targetBright); 
                }
                checkColorAndBright(&currentRGB, &targetRGB, &currentBright, &targetBright, fade_increment);
                set_pwm_channels(ledc_ch_R, ledc_ch_G,ledc_ch_B, currentRGB, currentBright);
            }else if(ledMode==GLITCH){
                
            }
        }
        uint16_t delay = refreshRate_ms - pdTICKS_TO_MS(xTaskGetTickCount()-startTick);
        //ESP_LOGD(TAG, "Led delay :%d state:%d, currentBright:%d", delay, state, currentBright); 
        vTaskDelay(pdMS_TO_TICKS(delay));
    }

	EXIT:
    vTaskDelete(NULL);
}

void init_3n_mosfet(int slot_num) {
	uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreate(rgb_ledc_task, "smartLed_task", 1024*4, &slot_num,12, NULL);

	ESP_LOGD(TAG,"pwmRGB_led task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
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

