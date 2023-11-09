#include "buttons.h"
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

extern uint8_t SLOTS_PIN_MAP[6][4];
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
}

static void IRAM_ATTR fall_handler(void *args)
{
	pwmEvent_t *tickVals = (pwmEvent_t *)args;

	tickVals->tick_fall = esp_timer_get_time();

	// if((abs((tickVals->tick_fall-tickVals->tick_rise)-tickVals->dTime)>15)&&((tickVals->tick_fall-tickVals->tick_rise)>2)){
	if ((tickVals->tick_fall - tickVals->tick_rise) > 2)
	{
		tickVals->flag = 1;
		tickVals->dTime = (tickVals->tick_fall - tickVals->tick_rise);
	}
}

void encoderPWM_task(void *arg)
{
	int num_of_slot = *(int *)arg;

	uint8_t rise_pin_num = SLOTS_PIN_MAP[num_of_slot][0];
	esp_rom_gpio_pad_select_gpio(rise_pin_num);
	gpio_set_direction(rise_pin_num, GPIO_MODE_INPUT);
	gpio_pulldown_en(rise_pin_num);
	gpio_pullup_dis(rise_pin_num);
	gpio_set_intr_type(rise_pin_num, GPIO_INTR_POSEDGE);

	uint8_t fall_pin_num = SLOTS_PIN_MAP[num_of_slot][1];
	esp_rom_gpio_pad_select_gpio(fall_pin_num);
	gpio_set_direction(fall_pin_num, GPIO_MODE_INPUT);
	gpio_pulldown_en(fall_pin_num);
	gpio_pullup_dis(fall_pin_num);
	gpio_set_intr_type(fall_pin_num, GPIO_INTR_NEGEDGE);

	pwmEvent_t tickVals;
	gpio_install_isr_service(0);
	gpio_isr_handler_add(rise_pin_num, rise_handler, (void *)&tickVals);
	gpio_isr_handler_add(fall_pin_num, fall_handler, (void *)&tickVals);

#define INCREMENTAL 0
#define ABSOLUTE 1
	uint8_t encoderMode = INCREMENTAL;
	if (strstr(me_config.slot_options[num_of_slot], "absolute") != NULL)
	{
		encoderMode = ABSOLUTE;
		ESP_LOGD(TAG, "pwmEncoder mode: absolute slot:%d", num_of_slot);
	}
	else
	{
		ESP_LOGD(TAG, "pwmEncoder mode: incremental slot:%d", num_of_slot);
	}

#define MIN_VAL 3
#define MAX_VAL 926
	int pole = MAX_VAL - MIN_VAL;
	int num_of_pos;
	if (strstr(me_config.slot_options[num_of_slot], "num_of_pos") != NULL)
	{
		num_of_pos = get_option_int_val(num_of_slot, "num_of_pos");
		if (num_of_pos <= 0)
		{
			ESP_LOGD(TAG, "pwmEncoder num_of_pos wrong format, set default slot:%d", num_of_slot);
			num_of_pos = 24; // default val
		}
	}
	else
	{
		num_of_pos = 24; // default val
	}
	ESP_LOGD(TAG, "pwmEncoder num_of_pos:%d slot:%d", num_of_pos, num_of_slot);
	int offset;
	int pos_length = pole / num_of_pos;
	int64_t raw_val, filtred_val;
	int current_pos, prev_pos = -1;

	while (tickVals.flag != 1)
	{
		// vTaskDelay(pdMS_TO_TICKS(1)); / portTICK_PERIOD_MS
		vTaskDelay(1 / portTICK_PERIOD_MS);
	}
	raw_val = tickVals.dTime;

	current_pos = raw_val / pos_length;
	offset = (raw_val % pos_length) + (pos_length / 2); //
	ESP_LOGD(TAG, "pwmEncoder first_val:%lld offset:%d pos_legth:%d", raw_val, offset, pos_length);

	#define ANTI_DEBOUNCE_INERATIONS 5
	int anti_deb_mass_index = 0;
	int val_mass[ANTI_DEBOUNCE_INERATIONS];

	while (1)
	{
		vTaskDelay(pdMS_TO_TICKS(10));
		if (tickVals.flag)
		{
			raw_val = tickVals.dTime + offset;

			val_mass[anti_deb_mass_index] = raw_val;
			anti_deb_mass_index++;
			if (anti_deb_mass_index == ANTI_DEBOUNCE_INERATIONS)
			{
				anti_deb_mass_index = 0;
			}
			int sum = 0;
			for (int i = 1; i < ANTI_DEBOUNCE_INERATIONS; i++)
			{
				if (abs(val_mass[i] - val_mass[i - 1]) < 3)
				{
					sum++;
				}
			}
			if (sum >= (ANTI_DEBOUNCE_INERATIONS - 1))
			{
				current_pos = raw_val / pos_length - 1;
				if (current_pos >= num_of_pos)
				{
					current_pos = 0;
				}
			}

			if (current_pos != prev_pos)
			{
				// printf("val_mas: %d %d %d %d %d \r\n",val_mass[0],val_mass[1],val_mass[2],val_mass[3],val_mass[4]);
				//printf("Current_pos: %d  raw_val:%d sum_val:%d\r\n", current_pos, (int)raw_val, sum);
				int str_len = strlen(me_config.device_name) + strlen("/encoder_") + 8;
				char *str = (char *)malloc(str_len * sizeof(char));
				char dir[20];
				if (encoderMode == ABSOLUTE)
				{
					sprintf(str, "%s/encoder_%d:%d", me_config.device_name, num_of_slot, current_pos);
				}
				else
				{
					int delta = abs(current_pos - prev_pos);
					if (delta < (num_of_pos / 2))
					{
						if (current_pos < prev_pos)
						{
							sprintf(dir, "+%d", delta);
						}
						else
						{
							sprintf(dir, "-%d", delta);
						}
					}
					else
					{
						delta = num_of_pos - delta;
						if (current_pos < prev_pos)
						{
							sprintf(dir, "-%d", delta);
						}
						else
						{
							sprintf(dir, "+%d", delta);
						}
					}

					sprintf(str, "%s/encoder_%d:%s", me_config.device_name, num_of_slot, dir);
				}
				report(str);
				free(str);
				prev_pos = current_pos;
			}
			tickVals.flag = 0;
		}
	}
}

void start_encoderPWM_task(int num_of_slot)
{

	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = num_of_slot;
	// int num_of_slot = *(int*) arg;
	xTaskCreate(encoderPWM_task, "encoderCalc", 1024 * 4, &t_slot_num, 1, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "pwmEncoder init ok: %d Heap usage: %lu free heap:%u", num_of_slot, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

//-------------------incremental encoder secttion--------------------------
void encoder_inc_task(void *arg)
{
	int num_of_slot = *(int *)arg;
	uint8_t a_pin_num = SLOTS_PIN_MAP[num_of_slot][0];
	uint8_t b_pin_num = SLOTS_PIN_MAP[num_of_slot][1];

	gpio_install_isr_service(0);
	rotary_encoder_info_t info = {0};

	uint8_t inverse  = 0;
	if (strstr(me_config.slot_options[num_of_slot], "inverse")!=NULL){
		inverse=1;
	}
	if(inverse){
		ESP_ERROR_CHECK(rotary_encoder_init(&info, b_pin_num, a_pin_num));
	}else{
		ESP_ERROR_CHECK(rotary_encoder_init(&info, a_pin_num, b_pin_num));
	}

	uint8_t absolute  = 0;
	if (strstr(me_config.slot_options[num_of_slot], "absolute")!=NULL){
		absolute=1;
	}
	
	uint8_t flag_custom_topic = 0;
	char *custom_topic=NULL;
	if (strstr(me_config.slot_options[num_of_slot], "custom_topic")!=NULL){
		custom_topic = get_option_string_val(num_of_slot,"custom_topic");
		ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
		flag_custom_topic=1;
	}

    if(flag_custom_topic==0){
		char *str = calloc(strlen(me_config.device_name)+strlen("/encoder_")+4, sizeof(char));
		sprintf(str, "%s/encoder_%d",me_config.device_name, num_of_slot);
		me_state.triggers_topic_list[me_state.triggers_topic_list_index]=str;
	}else{
		me_state.triggers_topic_list[me_state.triggers_topic_list_index]=custom_topic;
	}
	me_state.triggers_topic_list_index++;


	//QueueHandle_t event_queue = rotary_encoder_create_queue();
	//ESP_ERROR_CHECK(rotary_encoder_set_queue(&info, event_queue));

	int32_t pos, prev_pos=0;

	while (1)
	{
		// Wait for incoming events on the event queue.
		pos = info.state.position;

		int str_len;
		char *str;
		if(pos!=prev_pos){
			//ESP_LOGD(TAG, "Position %d %s",event.state.position, debugString);
			//ESP_LOGD(TAG, "report pos:%d",  pos);
			if(flag_custom_topic){
				str_len=strlen(custom_topic)+4;
				str = (char*)malloc(str_len * sizeof(char));
				if(absolute){
                    sprintf(str,"%s:%ld", custom_topic, pos);
                }else{
                    sprintf(str,"%s:%ld", custom_topic,  prev_pos - pos);
                }
			}else{
				str_len=strlen(me_config.device_name)+strlen("/encoder_")+8;
				str = (char*)malloc(str_len * sizeof(char));
				if(absolute){
					sprintf(str,"%s/encoder_%d:%ld", me_config.device_name, 0, pos);
				}else{
					sprintf(str,"%s/encoder_%d:%ld", me_config.device_name, 0, prev_pos - pos);
				}
			}
			report(str);
			free(str);
			prev_pos = pos;
		}
		vTaskDelay(pdMS_TO_TICKS(20));
	}
}


void start_encoder_inc_task(int num_of_slot)
{
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = num_of_slot;
	// int num_of_slot = *(int*) arg;
	xTaskCreate(encoder_inc_task, "encoder_inc_task", 1024 * 4, &t_slot_num, 1, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "encoder_inc_task init ok: %d Heap usage: %lu free heap:%u", num_of_slot, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}