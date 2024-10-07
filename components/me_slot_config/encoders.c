//#include "buttons.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"
#include <driver/mcpwm.h>

#include "reporter.h"
#include "stateConfig.h"
#include "rotary_encoder.h"

#include "esp_system.h"
#include "esp_log.h"
#include "me_slot_config.h"

#include "esp_rom_sys.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern uint8_t led_segment;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ENCODERS";
extern char debugString[200];
// uint64_t rTime, fTime;
// uint64_t dTime=0;
// uint8_t flag_calc;

typedef struct
{
	uint8_t flag;
	int64_t tick_rise;
	int64_t tick_fall;
	int64_t dTime;
} pwmEvent_t;

static void IRAM_ATTR rise_handler(void *args)
{
	pwmEvent_t *tickVals = (pwmEvent_t *)args;
	tickVals->tick_rise = esp_timer_get_time();

	//esp_rom_printf("rise_handler\n");
}

static void IRAM_ATTR fall_handler(void *args)
{
	//esp_rom_printf("fall_handler\n");
	pwmEvent_t *tickVals = (pwmEvent_t *)args;

	tickVals->tick_fall = esp_timer_get_time();

	// if((abs((tickVals->tick_fall-tickVals->tick_rise)-tickVals->dTime)>15)&&((tickVals->tick_fall-tickVals->tick_rise)>2)){
	if ((tickVals->tick_fall - tickVals->tick_rise) > 2){
		tickVals->flag = 1;
		tickVals->dTime = (tickVals->tick_fall - tickVals->tick_rise);
	}
}

void encoderPWM_task(void *arg)
{
	int slot_num = *(int *)arg;

	char str[255];

	uint8_t rise_pin_num = SLOTS_PIN_MAP[slot_num][0];
	esp_rom_gpio_pad_select_gpio(rise_pin_num);
	gpio_set_direction(rise_pin_num, GPIO_MODE_INPUT);
	gpio_pulldown_en(rise_pin_num);
	gpio_pullup_dis(rise_pin_num);
	gpio_set_intr_type(rise_pin_num, GPIO_INTR_POSEDGE);

	uint8_t fall_pin_num = SLOTS_PIN_MAP[slot_num][1];
	esp_rom_gpio_pad_select_gpio(fall_pin_num);
	gpio_set_direction(fall_pin_num, GPIO_MODE_INPUT);
	gpio_pulldown_en(fall_pin_num);
	gpio_pullup_dis(fall_pin_num);
	gpio_set_intr_type(fall_pin_num, GPIO_INTR_NEGEDGE);

	pwmEvent_t tickVals;
	tickVals.flag = 0;
	gpio_install_isr_service(0);
	gpio_isr_handler_add(rise_pin_num, rise_handler, (void *)&tickVals);
	gpio_isr_handler_add(fall_pin_num, fall_handler, (void *)&tickVals);

#define INCREMENTAL 0
#define ABSOLUTE 1
	uint8_t encoderMode = INCREMENTAL;
	if (strstr(me_config.slot_options[slot_num], "absolute") != NULL){
		encoderMode = ABSOLUTE;
		ESP_LOGD(TAG, "pwmEncoder mode: absolute slot:%d", slot_num);
	}else{
		ESP_LOGD(TAG, "pwmEncoder mode: incremental slot:%d", slot_num);
	}


	uint8_t float_output = 0;
	if (strstr(me_config.slot_options[slot_num], "float_output") != NULL){
		float_output = 1;
		ESP_LOGD(TAG, "float_output mode: %d", slot_num);
	}

	uint8_t zero_shift = 0;
	if (strstr(me_config.slot_options[slot_num], "zero_shift") != NULL){
		zero_shift = get_option_int_val(slot_num, "zero_shift");
		ESP_LOGD(TAG, "zero_shift: %d", zero_shift);
	}

#define MIN_VAL 3
#define MAX_VAL 926
	int pole = MAX_VAL - MIN_VAL;
	int num_of_pos;
	if (strstr(me_config.slot_options[slot_num], "num_of_pos") != NULL)	{
		num_of_pos = get_option_int_val(slot_num, "num_of_pos");
		if (num_of_pos <= 0){
			ESP_LOGD(TAG, "pwmEncoder num_of_pos wrong format, set default slot:%d", slot_num);
			num_of_pos = 24; // default val
		}
	}else{
		num_of_pos = 24; // default val
	}
	ESP_LOGD(TAG, "pwmEncoder num_of_pos:%d slot:%d", num_of_pos, slot_num);
	
	float pos_length = (float)pole / num_of_pos;
	uint16_t raw_val;
	int current_pos, prev_pos = -1;

	if (strstr(me_config.slot_options[slot_num], "encoder_topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "encoder_topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/encoder_0")+3];
		sprintf(t_str, "%s/encoder_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}

	ESP_LOGD(TAG, "Lets wait first interrupt");
	while (tickVals.flag != 1)
	{
		// vTaskDelay(pdMS_TO_TICKS(1)); / portTICK_PERIOD_MS
		vTaskDelay(1 / portTICK_PERIOD_MS);
	}
	
	raw_val = tickVals.dTime;
	current_pos = (raw_val / pos_length)+zero_shift;
	while(current_pos>=num_of_pos){
		current_pos -= num_of_pos;
	}

	int offset = raw_val;
	while(offset >= pos_length){
		offset -= pos_length;
	}
	offset = -(offset - (pos_length / 2)); //
	ESP_LOGD(TAG, "pwmEncoder first_val:%d offset:%d pos_legth:%f", raw_val, offset, pos_length);

	#define ANTI_DEBOUNCE_INERATIONS 3
	int anti_deb_mass_index = 0;
	int val_mass[ANTI_DEBOUNCE_INERATIONS];

	while (1){
		vTaskDelay(pdMS_TO_TICKS(10));
		if (tickVals.flag){
			raw_val = tickVals.dTime + offset;
		}else if((esp_timer_get_time()-tickVals.tick_rise)>1000){
			raw_val = 0;
		}

		//raw_val = raw_val + offset;
		//ESP_LOGD(TAG, "raw_val:%d", raw_val);
		while(raw_val > pole){
			raw_val = pole;
		}

		val_mass[anti_deb_mass_index] = raw_val;
		anti_deb_mass_index++;
		if (anti_deb_mass_index == ANTI_DEBOUNCE_INERATIONS){
			anti_deb_mass_index = 0;
		}
		int sum = 0;
		for (int i = 1; i < ANTI_DEBOUNCE_INERATIONS; i++){
			if (abs(val_mass[i] - val_mass[i - 1]) < 3)	{
				sum++;
			}
		}
		if (sum >= (ANTI_DEBOUNCE_INERATIONS - 1)){
			current_pos = (raw_val / pos_length)+zero_shift;
			while(current_pos>=num_of_pos){
				current_pos -= num_of_pos;
			}
		}

		//ESP_LOGD(TAG, "raw_val:%d current_pos:%d", raw_val, current_pos);

		if (current_pos != prev_pos){
			//ESP_LOGD(TAG, "raw_val:%d current_pos:%d", raw_val, current_pos);
			if (encoderMode == ABSOLUTE){
				if(float_output){
					sprintf(str, "%f", (float)current_pos/(num_of_pos-1));
				}else{
					sprintf(str, "%d", current_pos);
				}
			}else if (encoderMode == INCREMENTAL){
				int delta = abs(current_pos - prev_pos);
				if (delta < (num_of_pos / 2)){
					if (current_pos < prev_pos)	{
						sprintf(str, "-%d", delta);
					}else{
						sprintf(str, "+%d", delta);
					}
				}else{
					delta = num_of_pos - delta;
					if (current_pos < prev_pos)	{
						sprintf(str, "+%d", delta);
					}else{
						sprintf(str, "-%d", delta);
					}
				}
				//sprintf(str, "%s/encoder_%d:%s", me_config.deviceName, slot_num, dir);
			}
			report(str, slot_num);
			prev_pos = current_pos;
		}
		tickVals.flag = 0;
		
	}
}

void start_encoderPWM_task(int slot_num)
{

	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	// int slot_num = *(int*) arg;
	xTaskCreate(encoderPWM_task, "encoderCalc", 1024 * 4, &t_slot_num, 1, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "pwmEncoder init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

//-------------------incremental encoder secttion--------------------------
void encoder_inc_task(void *arg){
	int slot_num = *(int *)arg;
	uint8_t a_pin_num = SLOTS_PIN_MAP[slot_num][0];
	uint8_t b_pin_num = SLOTS_PIN_MAP[slot_num][1];

	gpio_install_isr_service(0);
	rotary_encoder_info_t info = {0};

	uint8_t inverse  = 0;
	if (strstr(me_config.slot_options[slot_num], "inverse")!=NULL){
		inverse=1;
	}
	if(inverse){
		ESP_ERROR_CHECK(rotary_encoder_init(&info, b_pin_num, a_pin_num));
	}else{
		ESP_ERROR_CHECK(rotary_encoder_init(&info, a_pin_num, b_pin_num));
	}

	uint8_t absolute  = 0;
	if (strstr(me_config.slot_options[slot_num], "absolute")!=NULL){
		absolute=1;
	}
	
	uint8_t flag_custom_topic = 0;
	char *custom_topic=NULL;
	if (strstr(me_config.slot_options[slot_num], "custom_topic")!=NULL){
		custom_topic = get_option_string_val(slot_num,"custom_topic");
		ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
		flag_custom_topic=1;
	}

    if(flag_custom_topic==0){
		char *str = calloc(strlen(me_config.deviceName)+strlen("/encoder_")+4, sizeof(char));
		sprintf(str, "%s/encoder_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=str;
	}else{
		me_state.trigger_topic_list[slot_num]=custom_topic;
	}



	//QueueHandle_t event_queue = rotary_encoder_create_queue();
	//ESP_ERROR_CHECK(rotary_encoder_set_queue(&info, event_queue));

	int32_t pos, prev_pos=0;

	while (1)
	{
		// Wait for incoming events on the event queue.
		pos = info.state.position;

		char str[40];
		if(pos!=prev_pos){

			if(absolute){
				sprintf(str,"%ld", pos);
			}else{
				sprintf(str,"%ld",prev_pos - pos);
			}

			report(str, slot_num);
			//vPortFree(str);
			prev_pos = pos;
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}


void start_encoder_inc_task(int slot_num)
{
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	// int slot_num = *(int*) arg;
	xTaskCreate(encoder_inc_task, "encoder_inc_task", 1024 * 4, &t_slot_num, 1, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "encoder_inc_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}