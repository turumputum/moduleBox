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
#include "driver/pcnt.h"
#include "driver/pulse_cnt.h"
#include "esp_system.h"
#include "esp_log.h"
#include "me_slot_config.h"
#include "executor.h"
#include "esp_rom_sys.h"
#include "driver/i2c.h"

#include <generated_files/gen_encoders.h>

#include <stdcommand.h>
#include <stdreport.h>

#include <manifest.h>
#include <mbdebug.h>

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "ENCODERS";

#define INCREMENTAL 0
#define ABSOLUTE 1




typedef struct
{
	uint8_t flag;
	int64_t tick_rise;
	int64_t tick_fall;
	int64_t dTime;
} pwmEvent_t;



static void IRAM_ATTR rise_handler(void *args){
	pwmEvent_t *tickVals = (pwmEvent_t *)args;
	tickVals->tick_rise = esp_timer_get_time();
}

static void IRAM_ATTR fall_handler(void *args){
	pwmEvent_t *tickVals = (pwmEvent_t *)args;
	tickVals->tick_fall = esp_timer_get_time();
	if ((tickVals->tick_fall - tickVals->tick_rise) > 2){
		tickVals->flag = 1;
		tickVals->dTime = (tickVals->tick_fall - tickVals->tick_rise);
	}
}

typedef struct __tag_PPMCONFIG
{
	uint8_t 				encoderMode;
	uint8_t 				float_output;
	uint8_t 				dirInverse;
	uint8_t 				zero_shift;
	uint8_t 				calibrationFlag;
	uint16_t 				MIN_VAL;
	uint16_t 				MAX_VAL;
	uint16_t 				deadZone;
	int16_t 				offset;
	uint16_t                refreshPeriod;
	float 					filterK;
	int 					pole;
	int 					numOfPos;
} PPMCONFIG, * PPPMCONFIG; 

/* 
	Модуль энкодера с фазово импульсной модуляцией(PPM)
*/
void configure_encoderPPM(PPPMCONFIG c, int slot_num)
{

	c->encoderMode = INCREMENTAL;
	if (strstr(me_config.slot_options[slot_num], "absolute") != NULL){
		/* Флаг определяет абслютный режим работы энкодера, 
			иначе - инкрементальный
        */
		c->encoderMode = get_option_flag_val(slot_num, "absolute");
		ESP_LOGD(TAG, "pwmEncoder mode: absolute slot:%d", slot_num);
	}else{
		ESP_LOGD(TAG, "pwmEncoder mode: incremental slot:%d", slot_num);
	}

	if (strstr(me_config.slot_options[slot_num], "floatOutput") != NULL){
		/* Флаг определяет значение с плавающей точкой,
		   иначе - целочисленное
		*/
		c->float_output = get_option_flag_val(slot_num, "floatOutput");
		ESP_LOGD(TAG, "float_output mode: %d", slot_num);
	}

	if (strstr(me_config.slot_options[slot_num], "dirInverse") != NULL){
		/* Флаг задаёт инверсию направления */
		c->dirInverse = get_option_flag_val(slot_num, "dirInverse");
		ESP_LOGD(TAG, "dirInverse slot: %d", slot_num);
	}

	if (strstr(me_config.slot_options[slot_num], "zeroShift") != NULL){
		/* Значение смещения нуля
		*/
		c->zero_shift = get_option_int_val(slot_num, "zeroShift", "", 10, 1, 4096);
		ESP_LOGD(TAG, "zero_shift: %d", c->zero_shift);
	}

	c->calibrationFlag = 0;
	if (strstr(me_config.slot_options[slot_num], "calibration") != NULL){
		/* Флаг задаёт необходимость калибровки */
		c->calibrationFlag = get_option_flag_val(slot_num, "calibration");
		ESP_LOGD(TAG, "calibrationFlag!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
	}

	c->filterK = 1.0;
	if (strstr(me_config.slot_options[slot_num], "filterK") != NULL){
		c->filterK = get_option_float_val(slot_num, "filterK",1.0);
		ESP_LOGD(TAG, "filterK: %f", c->filterK);
	}

	c->deadZone = 10;
	if (strstr(me_config.slot_options[slot_num], "deadZone") != NULL){
		c->deadZone = get_option_int_val(slot_num, "deadZone","", 10, 0, 4095);
		ESP_LOGD(TAG, "deadZone: %d", c->deadZone);
	}

	c->offset = 0;
	if (strstr(me_config.slot_options[slot_num], "offset") != NULL){
		c->offset = get_option_int_val(slot_num, "offset","", 0, 0, 4095);
		ESP_LOGD(TAG, "deadZone: %d", c->offset);
	}

	c->MIN_VAL = 15;
	if (strstr(me_config.slot_options[slot_num], "pwmMinVal") != NULL){
		/* Минимальное значение */
		c->MIN_VAL = get_option_int_val(slot_num, "pwmMinVal", "", 10, 1, 4096);
		ESP_LOGD(TAG, "MIN_VAL: %d", c->MIN_VAL);
	}

	c->MAX_VAL = 899;
	if (strstr(me_config.slot_options[slot_num], "pwmMaxVal") != NULL){
		/* Максимальное значение */
		c->MAX_VAL = get_option_int_val(slot_num, "pwmMaxVal", "", 10, 1, 4096);
		ESP_LOGD(TAG, "MAX_VAL: %d", c->MAX_VAL);
	}
	
	c->refreshPeriod = 25;
	if (strstr(me_config.slot_options[slot_num], "refreshRate") != NULL) {
		c->refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate","fps", 20, 1, 100));
		ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",c->refreshPeriod, slot_num);
	}

	c->pole = c->MAX_VAL - c->MIN_VAL;
	c->numOfPos = 24;
	if (strstr(me_config.slot_options[slot_num], "numOfPos") != NULL)	{
		/* Количество положений
		*/
		c->numOfPos = get_option_int_val(slot_num, "numOfPos", "", 10, 1, 4096);
		if (c->numOfPos <= 0){
			ESP_LOGD(TAG, "pwmEncoder numOfPos wrong format, set default slot:%d", slot_num);
			c->numOfPos = 24; // default val
		}
	}

	ESP_LOGD(TAG, "pwmEncoder numOfPos:%d slot:%d", c->numOfPos, slot_num);
	
	if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
		/* Определяет топик для MQTT сообщений */
    	custom_topic = get_option_string_val(slot_num, "topic", "/encoder_0");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/encoder_0")+3];
		sprintf(t_str, "%s/encoder_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}
}

// void encoderPPM_task(void *arg){
// 	PPMCONFIG		c 	= {0};
// 	int slot_num = *(int *)arg;

// 	char str[255];

// 	uint8_t rise_pin_num = SLOTS_PIN_MAP[slot_num][0];
// 	esp_rom_gpio_pad_select_gpio(rise_pin_num);
// 	gpio_set_direction(rise_pin_num, GPIO_MODE_INPUT);
// 	gpio_pulldown_en(rise_pin_num);
// 	gpio_pullup_dis(rise_pin_num);
// 	gpio_set_intr_type(rise_pin_num, GPIO_INTR_POSEDGE);

// 	uint8_t fall_pin_num = SLOTS_PIN_MAP[slot_num][1];
// 	esp_rom_gpio_pad_select_gpio(fall_pin_num);
// 	gpio_set_direction(fall_pin_num, GPIO_MODE_INPUT);
// 	gpio_pulldown_en(fall_pin_num);
// 	gpio_pullup_dis(fall_pin_num);
// 	gpio_set_intr_type(fall_pin_num, GPIO_INTR_NEGEDGE);

// 	pwmEvent_t tickVals;
// 	tickVals.flag = 0;
// 	gpio_install_isr_service(0);
// 	gpio_isr_handler_add(rise_pin_num, rise_handler, (void *)&tickVals);
// 	gpio_isr_handler_add(fall_pin_num, fall_handler, (void *)&tickVals);

// 	configure_encoderInc(&c, slot_num);

// 	float pos_length = (float)c.pole / c.numOfPos;
// 	int16_t raw_val;
// 	int current_pos, prev_pos = -1;

// 	ESP_LOGD(TAG, "Lets wait first interrupt");
// 	while (tickVals.flag != 1)
// 	{
// 		// vTaskDelay(pdMS_TO_TICKS(1)); / portTICK_PERIOD_MS
// 		vTaskDelay(1 / portTICK_PERIOD_MS);
// 	}
	
// 	raw_val = tickVals.dTime;
// 	current_pos = (raw_val / pos_length)+c.zero_shift;
// 	while(current_pos>=c.numOfPos){
// 		current_pos -= c.numOfPos;
// 	}

// 	ESP_LOGD(TAG, "pwmEncoder first_val:%d offset:%d pos_legth:%f", raw_val, c.offset, pos_length);

// 	#define ANTI_DEBOUNCE_INERATIONS 1
// 	int anti_deb_mass_index = 0;
// 	int val_mass[ANTI_DEBOUNCE_INERATIONS];
	
// 	float filtredVal = 0;
// 	float prew_filtredVal=-1;
// 	TickType_t lastWakeTime = xTaskGetTickCount(); 
// 	waitForWorkPermit(slot_num);

// 	while (1){
// 		vTaskDelay(pdMS_TO_TICKS(10));
// 		if (tickVals.flag){
// 			raw_val = tickVals.dTime + c.offset;
// 			//raw_val = tickVals.dTime + offset;
// 		}else if((esp_timer_get_time()-tickVals.tick_rise)>1000){
// 			raw_val = 0;
// 		}

// 		while(raw_val > c.pole){
// 			raw_val = c.pole;
// 		}
// 		if(raw_val > c.pole){
// 			raw_val -= c.pole;
// 		}else if(raw_val < 0){
// 			raw_val += c.pole;
// 		}

// 		if(raw_val<c.MIN_VAL){
// 			c.MIN_VAL = raw_val;
// 			c.pole = c.MAX_VAL - c.MIN_VAL;
// 			pos_length = (float)c.pole / c.numOfPos;
// 		}else if(raw_val>c.MAX_VAL){
// 			c.MAX_VAL = raw_val;
// 			c.pole = c.MAX_VAL - c.MIN_VAL;
// 			pos_length = (float)c.pole / c.numOfPos;
// 		}

// 		if((c.filterK<1)&&(abs(raw_val-prew_filtredVal)<c.pole/2)){
// 			filtredVal = filtredVal*(1-c.filterK) + raw_val*(c.filterK);
// 			//ESP_LOGD(TAG, "filtredVal:%f raw_val:%d", filtredVal, raw_val);
// 		}else{
// 			filtredVal = raw_val;
// 		}

// 		if(abs(filtredVal-prew_filtredVal)>c.deadZone){
// 			prew_filtredVal = filtredVal;
// 			current_pos = 0;
// 			float tmpVal = filtredVal;
// 			while(tmpVal > pos_length){
// 				current_pos++;
// 				tmpVal -= pos_length;
// 			}
// 			//ESP_LOGD(TAG, "obrezok: %f", tmpVal);
// 			//current_pos = ((raw_val- pos_length/2)/ pos_length)+zero_shift;
// 			while(current_pos>=c.numOfPos){
// 				current_pos -= c.numOfPos;
// 			}

// 			if(c.dirInverse){
// 				current_pos = c.numOfPos-current_pos;
// 			}
// 		}

// 		//ESP_LOGD(TAG, "raw_val:%d center:%d delta:%d", raw_val, (int)(current_pos*pos_length+pos_length/2), abs(raw_val-(current_pos*pos_length+pos_length/2)) );

// 		if (current_pos != prev_pos){
// 			//ESP_LOGD(TAG, "raw_val:%d current_pos:%d", raw_val, current_pos);
// 			if (c.encoderMode == ABSOLUTE){
// 				if(c.float_output){
// 					sprintf(str, "%f", (float)current_pos/(c.numOfPos-1));
// 				}else{
// 					sprintf(str, "%d", current_pos);
// 				}
// 			}else if (c.encoderMode == INCREMENTAL){
// 				int delta = abs(current_pos - prev_pos);
// 				if (delta < (c.numOfPos / 2)){
// 					if (current_pos < prev_pos)	{
// 						sprintf(str, "-%d", delta);
// 					}else{
// 						sprintf(str, "+%d", delta);
// 					}
// 				}else{
// 					delta = c.numOfPos - delta;
// 					if (current_pos < prev_pos)	{
// 						sprintf(str, "+%d", delta);
// 					}else{
// 						sprintf(str, "-%d", delta);
// 					}
// 				}
// 				//sprintf(str, "%s/encoder_%d:%s", me_config.deviceName, slot_num, dir);
// 			}
// 			report(str, slot_num);
// 			prev_pos = current_pos;
// 		}

// 		if(c.calibrationFlag){
// 			sprintf(str, "/calibration: dTime:%lld pos:%d delta:%d", tickVals.dTime, current_pos, abs(raw_val-(current_pos*pos_length+pos_length/2)));
// 			ESP_LOGD(TAG,"%s", str);
// 			report(str, slot_num);
// 		}

// 		tickVals.flag = 0;

// 		if (xTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(c.refreshPeriod)) == pdFALSE) {
//             ESP_LOGE(TAG, "Delay missed! Adjusting wake time.");
//             lastWakeTime = xTaskGetTickCount(); // Сброс времени пробуждения
//         }
		
// 	}
// }

// void start_encoderPPM_task(int slot_num)
// {

// 	uint32_t heapBefore = xPortGetFreeHeapSize();
// 	int t_slot_num = slot_num;
// 	// int slot_num = *(int*) arg;
// 	xTaskCreate(encoderPPM_task, "encoderCalc", 1024 * 4, &t_slot_num, 1, NULL);
// 	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

// 	ESP_LOGD(TAG, "pwmEncoder init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
// }

//-------------------incremental encoder secttion--------------------------
// void encoder_inc_task(void *arg){
// 	int slot_num = *(int *)arg;
// 	uint8_t a_pin_num = SLOTS_PIN_MAP[slot_num][0];
// 	uint8_t b_pin_num = SLOTS_PIN_MAP[slot_num][1];

// 	gpio_install_isr_service(0);
// 	rotary_encoder_info_t info = {0};

// 	uint8_t inverse  = 0;
// 	if (strstr(me_config.slot_options[slot_num], "inverse")!=NULL){
// 		inverse=1;
// 	}
// 	if(inverse){
// 		ESP_ERROR_CHECK(rotary_encoder_init(&info, b_pin_num, a_pin_num));
// 	}else{
// 		ESP_ERROR_CHECK(rotary_encoder_init(&info, a_pin_num, b_pin_num));
// 	}

// 	uint8_t absolute  = 0;
// 	if (strstr(me_config.slot_options[slot_num], "absolute")!=NULL){
// 		absolute=1;
// 	}
	
// 	uint8_t flag_custom_topic = 0;
// 	char *custom_topic=NULL;
// 	if (strstr(me_config.slot_options[slot_num], "topic")!=NULL){
// 		custom_topic = get_option_string_val(slot_num,"topic");
// 		ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
// 		flag_custom_topic=1;
// 	}

//     if(flag_custom_topic==0){
// 		char *str = calloc(strlen(me_config.deviceName)+strlen("/encoder_")+4, sizeof(char));
// 		sprintf(str, "%s/encoder_%d",me_config.deviceName, slot_num);
// 		me_state.trigger_topic_list[slot_num]=str;
// 	}else{
// 		me_state.trigger_topic_list[slot_num]=custom_topic;
// 	}



// 	//QueueHandle_t event_queue = rotary_encoder_create_queue();
// 	//ESP_ERROR_CHECK(rotary_encoder_set_queue(&info, event_queue));

// 	int32_t pos, prev_pos=0;

// 	while (1)
// 	{
// 		// Wait for incoming events on the event queue.
// 		pos = info.state.position;

// 		char str[40];
// 		if(pos!=prev_pos){

// 			if(absolute){
// 				sprintf(str,"%ld", pos);
// 			}else{
// 				sprintf(str,"%ld",prev_pos - pos);
// 			}

// 			report(str, slot_num);
// 			//vPortFree(str);
// 			prev_pos = pos;
// 		}
// 		vTaskDelay(pdMS_TO_TICKS(20));
// 	}
// }
//-------------------incremental encoder secttion--------------------------
typedef enum{
	INCCMD_reset = 0,
} INCCMD;

typedef struct __tag_INCCONFIG
{
	uint8_t 				mode;
	uint8_t 				floatOutput;
	uint8_t 				dirInverse;
	uint8_t					linearCounter;
	int16_t 				zeroShift;
	int32_t 				minVal;
	int32_t 				maxVal;
	uint16_t                refreshPeriod;
	int 					pole;
	uint16_t 				divider;
	uint16_t				glitchFilter;

	int                     report;

	STDCOMMANDS               cmds;
} INCCONFIG, * PINCCONFIG; 

/* 
	Модуль для работы с инкрементальными энкодерами, обычно оптический.
*/
void configure_encoderInc(PINCCONFIG c, int slot_num)
{

	c->mode = INCREMENTAL;
	/* Флаг определяет абслютный режим работы энкодера, 
		По умолчанию  - инкрементальный
	*/
	c->mode = get_option_flag_val(slot_num, "absolute");
	ESP_LOGD(TAG, "Encoder slotNum:%d mode:%s", slot_num, c->mode? "incremental" : "absolute");

	/* Флаг настраивает выходные значения в виде числа с плавающей точкой от 0.0 до 1.0 
		по умолчания - целочисленное
	*/
	c->floatOutput = get_option_flag_val(slot_num, "floatOutput");
	if(c->floatOutput){
		ESP_LOGD(TAG, "Encoder slotNum:%d  float_output", slot_num);
	}

	/* Флаг задаёт инверсию направления */
	c->dirInverse = get_option_flag_val(slot_num, "dirInverse");
	ESP_LOGD(TAG, "Encoder slotNum:%d dirInverse", slot_num);

	/* Флаг, режим работы энкода в виде линейного счетчика при достижении максимального и минимальных значений счет будет остановлен, 
		По умолчанию - счетчик работает в зациклен, и при достижении минимального значения переходит с камсимальному и на оборот
	*/
	c->linearCounter = get_option_flag_val(slot_num, "linearCounter");
	ESP_LOGD(TAG, "Encoder slotNum:%d mode:%s", slot_num, c->linearCounter? "linear" : "circular");

	/* Значение аппаратного фильтра
		- еденицы измерения наносекунды
	*/
	c->glitchFilter = get_option_int_val(slot_num, "glitchFilter", "ns", 800, 1, 4095);
	ESP_LOGD(TAG, "Encoder slotNum:%d glitchFilter:%d ns", slot_num, c->glitchFilter);


	/* Значение смещения нуля после делителя
	*/
	c->zeroShift = get_option_int_val(slot_num, "zeroShift", "", 0, INT16_MIN, INT16_MAX);
	ESP_LOGD(TAG, "Encoder slotNum:%d zero_shift: %d", slot_num, c->zeroShift);


	/* Минимальное значение */
	c->minVal = get_option_int_val(slot_num, "minVal", "", 0, INT32_MIN, INT32_MAX);
	ESP_LOGD(TAG, "MIN_VAL: %ld", c->minVal);


	/* Максимальное значение
	*/
	c->maxVal = get_option_int_val(slot_num, "maxVal", "", 4096, INT32_MIN, INT32_MAX);
	ESP_LOGD(TAG, "maxVal: %ld", c->maxVal);

	
	/* Период проверки значений
		- еденицы измерения раз в секунду
	*/
	c->refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate","fps", 20, 1, 100));
	ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",c->refreshPeriod, slot_num);
	
	c->pole = (c->maxVal - c->minVal)+1;

	/* Делитель*/
	c->divider = get_option_int_val(slot_num, "divider", "", 4, 1, UINT16_MAX);
	ESP_LOGD(TAG, "divider:%d for slotNum:%d", c->divider, slot_num);

	
	if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
		/* Определяет топик для MQTT сообщений */
    	custom_topic = get_option_string_val(slot_num, "topic", "/encoder_0");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/encoder_0")+3];
		sprintf(t_str, "%s/encoder_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}

	stdcommand_init(&c->cmds, slot_num);
    /* Обнуляет счетчик
    */
    stdcommand_register(&c->cmds, INCCMD_reset, "reset", PARAMT_none);

	/* Рапортует текущее значение
	*/
	c->report = stdreport_register(RPTT_string, slot_num, "", "");
}

void encoder_inc_task(void *arg){
    int slot_num = *(int *)arg;

	INCCONFIG		c = {0};
	configure_encoderInc(&c, slot_num);

	me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    uint8_t a_pin_num = SLOTS_PIN_MAP[slot_num][0];
    uint8_t b_pin_num = SLOTS_PIN_MAP[slot_num][1];

    //ESP_LOGD(TAG, "encoder_inc_task slot_num: %d, a_pin_num: %d, b_pin_num: %d", slot_num, a_pin_num, b_pin_num);

    pcnt_unit_config_t unit_config = {
        .high_limit = INT16_MAX,
        .low_limit = INT16_MIN,
		.flags.accum_count = true, // accumulate the counter value
    };
    pcnt_unit_handle_t pcnt_unit = NULL;
    esp_err_t err = pcnt_new_unit(&unit_config, &pcnt_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_unit failed: %d", err);
        return;
    }

	//ESP_LOGI(TAG, "set glitch filter");
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = c.glitchFilter,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(pcnt_unit, &filter_config));

    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = a_pin_num,
        .level_gpio_num = b_pin_num,
    };

	pcnt_channel_handle_t pcnt_chan_a = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_a_config, &pcnt_chan_a));

	pcnt_chan_config_t chan_b_config = {
        .edge_gpio_num = b_pin_num,
        .level_gpio_num = a_pin_num,
    };
    pcnt_channel_handle_t pcnt_chan_b = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(pcnt_unit, &chan_b_config, &pcnt_chan_b));

	//ESP_LOGI(TAG, "set edge and level actions for pcnt channels");
	if(!c.dirInverse){
		ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
		ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
		ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    	ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
	}else{
		ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan_a, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
		ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan_b, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
		ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan_a, PCNT_CHANNEL_LEVEL_ACTION_INVERSE, PCNT_CHANNEL_LEVEL_ACTION_KEEP));
    	ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan_b, PCNT_CHANNEL_LEVEL_ACTION_INVERSE, PCNT_CHANNEL_LEVEL_ACTION_KEEP));
	}
	//ESP_LOGI(TAG, "enable pcnt unit");
    ESP_ERROR_CHECK(pcnt_unit_enable(pcnt_unit));
    //ESP_LOGI(TAG, "clear pcnt unit");
    ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt_unit));
    //ESP_LOGI(TAG, "start pcnt unit");
    ESP_ERROR_CHECK(pcnt_unit_start(pcnt_unit));
	pcnt_unit_add_watch_point(pcnt_unit, INT16_MAX);
	pcnt_unit_add_watch_point(pcnt_unit, INT16_MIN);

    int32_t rawVal = 0, prewRawVal=0; //pcnt coint in int16 MAX-MIN range
    int32_t count = 0; // global accumulated count
	int32_t pos = 0; //after divider
	int32_t prev_pos = INT32_MIN;

	TickType_t lastWakeTime = xTaskGetTickCount();

	waitForWorkPermit(slot_num);

    while (1)
    {
		command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            //ESP_LOGD(TAG, "Input command %s for slot:%d", msg.str, msg.slot_num);
            char* payload = NULL;
            char* cmd = msg.str;
            if(strstr(cmd, "reset")!=NULL){
				pcnt_unit_clear_count(pcnt_unit);
            }
		}

        err = pcnt_unit_get_count(pcnt_unit, &rawVal);
		if (err != ESP_OK) {
            ESP_LOGE(TAG, "pcnt_get_counter_value failed: %d", err);
            return;
		}

		int32_t delta= rawVal - prewRawVal;
		if (abs(delta) > (INT16_MAX/2)) {
			// Произошло переполнение
			if(delta>0){
				//переполнение в отрицательной зоне
				delta = rawVal -(prewRawVal - INT16_MIN);
				//ESP_LOGD(TAG, "overload in negative zone");
			}else{
				delta = rawVal - (prewRawVal - INT16_MAX);
				//ESP_LOGD(TAG, "overload in positive zone");
			}
		}
		
		//ESP_LOGD(TAG, "RAW_val:%ld", rawCount);
		if(c.dirInverse){
			count += delta;
		}else{
			count -= delta;	
		}
		prewRawVal = rawVal;

		pos = count / c.divider;
		pos+=c.zeroShift;
		
		if(c.linearCounter){
			if(pos<c.minVal){
				pos = c.minVal;
				count = c.minVal;
				//prewRawVal = c.minVal;
			}else if(pos>c.maxVal){
				pos = c.maxVal;
				count = c.maxVal*c.divider;
				//prewRawVal = c.maxVal;
			}
		}else{
			while(pos<c.minVal){
				pos=pos+c.pole;
			}
			while(pos>c.maxVal){
				pos=pos-c.pole;
			}
		}
		

        if (pos != prev_pos) {
            char str[40];

            if (c.mode == ABSOLUTE) {
				
                sprintf(str, "%ld", pos);
            } else {
				int32_t diff = pos - prev_pos;
                sprintf(str, "%ld", diff);
            }

			stdreport_s(c.report, &str);
            //report(str, slot_num);
			//ESP_LOGD(TAG,"Report:%s",str);
            prev_pos = pos;
        }

        vTaskDelayUntil(&lastWakeTime, c.refreshPeriod);
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
const char * get_manifest_encoders(){
	return manifesto;
}


//--------------------------------------AS5600------------------------------------

typedef struct __tag_AS5600CONFIG
{
	uint8_t 				encoderMode;
	uint8_t 				floatOutput;
	uint8_t 				dirInverse;
	uint8_t 				zeroShift;
	int32_t 				minVal;
	int32_t 				maxVal;
	uint16_t                refreshPeriod;
	float					filterK;
	uint16_t				deadZone;
	uint16_t				numOfPos;
	int 					pole;
	uint16_t 				divider;

	int 					absReport;
	int						incReport;
	int 					floatReport;
} AS5600CONFIG, * PAS5600CONFIG; 

/* 
	Модуль для работы с инкрементальными энкодерами, обычно оптический.
*/
void configure_encoderAS5600(PAS5600CONFIG c, int slot_num)
{

	c->encoderMode = INCREMENTAL;
	/* Флаг определяет абслютный режим работы энкодера, 
		По умолчанию  - инкрементальный
	*/
	c->encoderMode = get_option_flag_val(slot_num, "absolute");
	ESP_LOGD(TAG, "[encoder_%d] mode:%s", slot_num, c->encoderMode? "incremental" : "absolute");

	/* Флаг натраивает выходные значения в виде числа с плавающей точкой,
		иначе - целочисленное
	*/
	c->floatOutput = get_option_flag_val(slot_num, "floatOutput");
	ESP_LOGD(TAG, "[encoder_%d]  float_output", slot_num);

	/* Флаг задаёт инверсию направления */
	c->dirInverse = get_option_flag_val(slot_num, "dirInverse");
	ESP_LOGD(TAG, "[encoder_%d] dir:%s", slot_num, c->dirInverse ? "normal" : "inverse");

	/* Значение зоны не чуствительности
	*/
	c->deadZone = get_option_int_val(slot_num, "deadZone", "", 0, 0, INT16_MAX);
	ESP_LOGD(TAG, "[encoder_%d] deadZone: %d", slot_num, c->deadZone);

	/* Значение смещения нуля после делителя
	*/
	c->zeroShift = get_option_int_val(slot_num, "zeroShift", "", 0, INT16_MIN, INT16_MAX);
	ESP_LOGD(TAG, "[encoder_%d] zero_shift: %d", slot_num, c->zeroShift);

	/* Значение смещения нуля после делителя
	*/
	c->filterK = get_option_float_val(slot_num, "filterK", 1.0);
	ESP_LOGD(TAG, "[encoder_%d] filterK: %f", slot_num, c->filterK);

	c->minVal = 0;
	c->maxVal = 4095;
	c->pole = c->maxVal - c->minVal;

	/* Количество сегментов на окружности
	*/
	c->numOfPos = get_option_int_val(slot_num, "numOfPos", "", 24, 0, 4096);
	ESP_LOGD(TAG, "[encoder_%d] numOfPos:%d ", slot_num, c->numOfPos);

	
	/* Период проверки значений
		- еденицы измерения раз в секунду
	*/
	c->refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate","fps", 20, 1, 100));
	ESP_LOGD(TAG, "[encoder_%d] refreshPeriod:%d", slot_num,c->refreshPeriod);
	
	

	/* Делитель*/
	c->divider = get_option_int_val(slot_num, "divider", "", 4, 1, UINT16_MAX);
	ESP_LOGD(TAG, "[encoder_%d] divider:%d", slot_num, c->divider);

	
	if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
		/* Определяет топик для MQTT сообщений */
    	custom_topic = get_option_string_val(slot_num, "topic", "/encoder_0");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/encoder_0")+3];
		sprintf(t_str, "%s/encoder_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}


	/* Рапортует текущее абсолютное положение
	*/
	c->absReport = stdreport_register(RPTT_string, slot_num, "", "abs");

	/* Рапортует текущее инкрементальное приращение
	*/
	c->incReport = stdreport_register(RPTT_string, slot_num, "", "inc");

	/* Рапортует текущее отношение в формате числа с плавающей точкой
	*/
	c->floatReport = stdreport_register(RPTT_string, slot_num, "", "float");

}

// AS5600 I2C registers
#define AS5600_I2C_ADDRESS 0x36
#define AS5600_RAW_ANGLE_REG 0x0C
#define AS5600_ANGLE_REG 0x0E
#define AS5600_STATUS_REG 0x0B
#define AS5600_AGC_REG 0x1A
#define AS5600_MAGNITUDE_REG 0x1B

#define I2C_MASTER_TIMEOUT_MS 100

esp_err_t checkMagnetStatus(int i2c_num, int slot_num){
	// Test AS5600 connection by reading status register
	esp_err_t ret = ESP_OK;
	uint8_t status;
	ret = i2c_master_write_read_device(i2c_num, AS5600_I2C_ADDRESS, 
										&(uint8_t){AS5600_STATUS_REG}, 1, 
										&status, 1, 
										I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
	if (ret != ESP_OK) {
		char tmpStr[100];
		sprintf(tmpStr, "AS5600 not found on slot:%d task terminated", slot_num);
		ESP_LOGE(TAG, "%s", tmpStr);
		mblog(0, tmpStr);
		vTaskDelete(NULL);
	}
	
	// Check magnetic field status
	bool magnet_detected = (status & 0x20) != 0;  // Bit 5: MD
	bool magnet_too_low = (status & 0x10) != 0;   // Bit 4: ML
	bool magnet_too_high = (status & 0x08) != 0;  // Bit 3: MH
	

	if (!magnet_detected) {
		char tmpStr[100];
		if (magnet_too_low) {
			sprintf(tmpStr,"AS5600 slot:%d - magnet field too weak! task terminated", slot_num);
		} else if (magnet_too_high) {
			printf(tmpStr, "AS5600 slot:%d - magnet field too strong! task terminated", slot_num);
		} else {
			printf(tmpStr, "AS5600 slot:%d - magnet not found! task terminated", slot_num);
		}
		ESP_LOGE(TAG,"%s", tmpStr);
		mblog(0, tmpStr);
		vTaskDelete(NULL);
	}
	return ret;
}

void encoderAS5600_task(void *arg)
{
	AS5600CONFIG c = {0};
	int slot_num = *(int *)arg;
	
	configure_encoderAS5600(&c, slot_num);

	char str[255];
	
	// I2C initialization
	uint8_t sda_pin = SLOTS_PIN_MAP[slot_num][0];
	uint8_t scl_pin = SLOTS_PIN_MAP[slot_num][1];
	gpio_num_t led_pin = SLOTS_PIN_MAP[slot_num][2];

	esp_rom_gpio_pad_select_gpio(led_pin);
	gpio_set_direction(led_pin, GPIO_MODE_OUTPUT);
	gpio_set_level(led_pin, 1);
	
	i2c_config_t conf = {
		.mode = I2C_MODE_MASTER,
		.sda_io_num = sda_pin,
		.scl_io_num = scl_pin,
		.sda_pullup_en = GPIO_PULLUP_ENABLE,
		.scl_pullup_en = GPIO_PULLUP_ENABLE,
		.master.clk_speed = 100000,
	};
	
	char tmpStr[100];

	int i2c_num = me_state.free_i2c_num;
	me_state.free_i2c_num++;
	if(i2c_num == I2C_NUM_MAX) {
		sprintf(tmpStr, "No free I2C driver for slot:%d task terminated", slot_num);
		ESP_LOGE(TAG, "%s", tmpStr);
		mblog(0, tmpStr);
		vTaskDelete(NULL);
	}
	
	i2c_param_config(i2c_num, &conf);
	esp_err_t ret = i2c_driver_install(i2c_num, conf.mode, 0, 0, 0);
	if (ret == ESP_OK) {
		ESP_LOGI(TAG, "I2C_%d initialized for AS5600 slot:%d", i2c_num, slot_num);
	} else {
		sprintf(tmpStr,  "Failed to initialize I2C_%d for slot:%d", i2c_num, slot_num);
		ESP_LOGE(TAG, "%s", tmpStr);
		mblog(0, tmpStr);
		vTaskDelete(NULL);
	}
	
	float pos_length = (float)c.pole / c.numOfPos;
	uint16_t raw_angle;
	int current_pos=0, prev_pos = -1;

	checkMagnetStatus(i2c_num, slot_num);

	// Read magnetic field magnitude
	uint8_t magnitude_data[2];
	ret = i2c_master_write_read_device(i2c_num, AS5600_I2C_ADDRESS,
									   &(uint8_t){AS5600_MAGNITUDE_REG}, 1,
									   magnitude_data, 2,
									   I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
	if (ret == ESP_OK) {
		uint16_t magnitude = ((uint16_t)magnitude_data[0] << 8) | magnitude_data[1];
		magnitude &= 0x0FFF; // Mask to 12 bits
		ESP_LOGI(TAG, "AS5600 slot:%d - сила магнитного поля: %d", slot_num, magnitude);
	}
	
	float filtredVal = 0;
	float prew_filtredVal = -1;
	TickType_t lastWakeTime = xTaskGetTickCount();
	int checkTick = 0;

	waitForWorkPermit(slot_num);
	
	while (1) {
		if(checkTick>1000){
			checkMagnetStatus(i2c_num, slot_num);
			checkTick = 0;
		}else{
			checkTick++;
		}

		// Read raw angle from AS5600
		uint8_t angle_reg = AS5600_RAW_ANGLE_REG;
		uint8_t angle_data[2];
		
		ret = i2c_master_write_read_device(i2c_num, AS5600_I2C_ADDRESS,
										   &angle_reg, 1,
										   angle_data, 2,
										   I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
		
		// if (ret != ESP_OK) {
		// 	ESP_LOGW(TAG, "Failed to read AS5600 angle on slot:%d", slot_num);
		// 	vTaskDelay(pdMS_TO_TICKS(10));
		// 	continue;
		// }
		
		// Combine bytes and convert to 12-bit value (0-4095)
		raw_angle = ((uint16_t)angle_data[0] << 8) | angle_data[1];
		raw_angle &= 0x0FFF; // Mask to 12 bits
		
		// Apply offset
		//ESP_LOGD(TAG, "rawVal:%d angle on slot:%d", raw_angle, slot_num);
		int16_t adjusted_angle = raw_angle + c.zeroShift;
		if(adjusted_angle < 0) {
			adjusted_angle += 4096;
		}
		if(adjusted_angle >= 4096) {
			adjusted_angle -= 4096;
		}
		
		// Apply filtering
		if ((c.filterK < 1) && (abs(adjusted_angle - prew_filtredVal) < 2048)) {
			filtredVal = filtredVal * (1 - c.filterK) + adjusted_angle * c.filterK;
		} else {
			filtredVal = adjusted_angle;
		}
		
		// Check dead zone
		if (abs(filtredVal - prew_filtredVal) > c.deadZone) {
			prew_filtredVal = filtredVal;
			gpio_set_level(led_pin, (int)filtredVal % 2);

			// Calculate position based on filtered value

			current_pos = filtredVal/pos_length;
			if(current_pos>=c.numOfPos){
				current_pos -=c.numOfPos;
			}

			//ESP_LOGD(TAG, "current_pos:%d filtredVal:%f sector;%f",current_pos, filtredVal, pos_length);
			if (c.dirInverse) {
				current_pos = c.numOfPos - current_pos;
			}
		}

		
		// Report changes
		if (current_pos != prev_pos) {
			//ESP_LOGD(TAG, "current_pos:%d prev_pos:%d slot:%d", current_pos, prev_pos, slot_num);
			if (c.encoderMode == ABSOLUTE) {
				if (c.floatOutput) {
					sprintf(str, "%f", (float)current_pos / (c.numOfPos - 1));
					stdreport_s(c.floatReport, str);
				} else {
					sprintf(str, "%d", current_pos);
					stdreport_s(c.absReport, str);
				}
				
			} else if (c.encoderMode == INCREMENTAL) {
				int delta = abs(current_pos - prev_pos);
				if (delta < (c.numOfPos / 2)) {
					if (current_pos < prev_pos) {
						sprintf(str, "-%d", delta);
					} else {
						sprintf(str, "+%d", delta);
					}
				} else {
					delta = c.numOfPos - delta;
					if (current_pos < prev_pos) {
						sprintf(str, "+%d", delta);
					} else {
						sprintf(str, "-%d", delta);
					}
				}
				stdreport_s(c.incReport, str);
			}
			
			//report(str, slot_num);
			prev_pos = current_pos;
		}
			
		if (xTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(c.refreshPeriod)) == pdFALSE) {
			ESP_LOGE(TAG, "AS5600 delay missed! Adjusting wake time slot:%d", slot_num);
			lastWakeTime = xTaskGetTickCount();
		}
	}
}

void start_encoderAS5600_task(int slot_num)
{
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	
	xTaskCreate(encoderAS5600_task, "encoderAS5600", 1024 * 4, &t_slot_num, 1, NULL);
	
	ESP_LOGD(TAG, "AS5600 encoder init ok: %d Heap usage: %lu free heap:%u", 
			 slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}