#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/ledc.h"
#include "esp_log.h"

#include "3n_mosfet.h"

#include "stateConfig.h"

extern uint8_t SLOTS_PIN_MAP[6][4];

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
//#define LEDC_OUTPUT_IO          (10) // Define the output GPIO
//#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_8_BIT // Set duty resolution to 13 bits
#define LEDC_DUTY               (4095) // Set duty to 50%. ((2 ** 13) - 1) * 50% = 4095
#define LEDC_FREQUENCY          (5000) // Frequency in Hertz. Set frequency at 5 kHz

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "3n_MOSFET";

extern configuration me_config;
extern stateStruct me_state;

int R=0;
int G=0;
int B=0;

uint8_t flag_glitch_task=0;
uint8_t glitchVal;

const uint8_t gamma8[256] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 3,
		3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18,
		19, 19, 20, 20, 21, 21, 22, 22, 23, 24, 24, 25, 25, 26, 27, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36, 37, 38, 39, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
		50, 51, 52, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 66, 67, 68, 69, 70, 72, 73, 74, 75, 77, 78, 79, 81, 82, 83, 85, 86, 87, 89, 90, 92, 93, 95, 96, 98, 99, 101, 102,
		104, 105, 107, 109, 110, 112, 114, 115, 117, 119, 120, 122, 124, 126, 127, 129, 131, 133, 135, 137, 138, 140, 142, 144, 146, 148, 150, 152, 154, 156, 158, 160, 162, 164,
		167, 169, 171, 173, 175, 177, 180, 182, 184, 186, 189, 191, 193, 196, 198, 200, 203, 205, 208, 210, 213, 215, 218, 220, 223, 225, 228, 231, 233, 236, 239, 241, 244, 247,
		249, 252, 255 };

void set_pwm_channels(int slot_num, int ch_1, int ch_2, int ch_3){
	ESP_LOGI(TAG, "Set PWM channels: %d %d %d", ch_1,ch_2,ch_3);
	ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_0, gamma8[ch_1]);
	ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_1, gamma8[ch_2]);
	ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_2, gamma8[ch_3]);
	ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_0);
	ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_1);
	ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_2);
}

void init_3n_mosfet(int slot_num) {
	uint32_t heapBefore = xPortGetFreeHeapSize();

	ledc_timer_config_t ledc_timer = {
			.speed_mode = LEDC_MODE,
			.timer_num = LEDC_TIMER,
			.duty_resolution = LEDC_DUTY_RES,
			.freq_hz = LEDC_FREQUENCY,  // Set output frequency at 5 kHz
			.clk_cfg = LEDC_AUTO_CLK };
	ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

	// Prepare and then apply the LEDC PWM channel configuration
	ledc_channel_config_t ledc_ch_1 = {
			.speed_mode = LEDC_MODE,
			.channel = LEDC_CHANNEL_0,
			.timer_sel = LEDC_TIMER,
			.intr_type = LEDC_INTR_DISABLE,
			.gpio_num = SLOTS_PIN_MAP[slot_num][0],
			.duty = 0, // Set duty to 0%
			.hpoint = 0 };

	ledc_channel_config_t ledc_ch_2 = {
				.speed_mode = LEDC_MODE,
				.channel = LEDC_CHANNEL_1,
				.timer_sel = LEDC_TIMER,
				.intr_type = LEDC_INTR_DISABLE,
				.gpio_num = SLOTS_PIN_MAP[slot_num][1],
				.duty = 0, // Set duty to 0%
				.hpoint = 0 };

	ledc_channel_config_t ledc_ch_3 = {
				.speed_mode = LEDC_MODE,
				.channel = LEDC_CHANNEL_2,
				.timer_sel = LEDC_TIMER,
				.intr_type = LEDC_INTR_DISABLE,
				.gpio_num = SLOTS_PIN_MAP[slot_num][2],
				.duty = 0, // Set duty to 0%
				.hpoint = 0 };


	ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch_1));
	ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch_2));
	ESP_ERROR_CHECK(ledc_channel_config(&ledc_ch_3));

	//---add action to topic list---
	char *str = calloc(strlen(me_config.device_name) + 12, sizeof(char));
	//sprintf(str, "%s/setRGB_%d", me_config.device_name, slot_num);
	sprintf(str, "%s/set", me_config.device_name);
	me_state.action_topic_list[me_state.action_topic_list_index] = str;
	me_state.action_topic_list_index++;
	char *str2 = calloc(strlen(me_config.device_name) + 12, sizeof(char));
	//sprintf(str2, "%s/setGlitch_%d", me_config.device_name, slot_num);
	sprintf(str2, "%s/glitch", me_config.device_name);
	me_state.action_topic_list[me_state.action_topic_list_index] = str2;
	me_state.action_topic_list_index++;


	ESP_LOGD(TAG, "3n_mosfet module inited for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());

}

void setRGB(int slot_num, char* payload){
	ESP_LOGD(TAG, "Set RGB for slot:%d val:%s",slot_num, payload);
	char *rest;
	char *tok;
	if(strstr(payload, " ")!=NULL){
		tok = strtok_r(payload, " ", &rest);
		R = atoi(tok);
		tok = strtok_r(NULL, " ", &rest);
		G = atoi(tok);
		B = atoi(rest);

		if (R > 255) {
			do {
				R -= 255;
			} while (R > 255);
		}
		if (G > 255) {
			do {
				G -= 255;
			} while (G > 255);
		}
		if (B > 255) {
			do {
				B -= 255;
			} while (B > 255);
		}
		set_pwm_channels(slot_num, R, G, B);
	}
}

void glitch_task(){
	flag_glitch_task=1;
	int state=0;
	int delay=rand()%(glitchVal*2) +10;
	while(1){
		if(state==0){
			set_pwm_channels(0, 0, 0, 0);
		}else{
			set_pwm_channels(0, R, G, B);
		}
		delay=rand()%glitchVal +glitchVal;
		state=!state;
		vTaskDelay(pdMS_TO_TICKS(delay));

		if(flag_glitch_task==0){
			vTaskDelete(NULL);
		}
	}
}

void setGlitch(int slot_num, char* payload){
	int val = atoi(payload);
	if(val==0){
		flag_glitch_task=0;
	}else if(flag_glitch_task!=1){
		xTaskCreate(glitch_task, "glitch", 1024 * 2, NULL, configMAX_PRIORITIES - 8, NULL);
	}
	glitchVal=val;
}
