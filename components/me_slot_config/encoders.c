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

static void IRAM_ATTR rise_handler(void *args){
	pwmEvent_t *tickVals = (pwmEvent_t *)args;
	tickVals->tick_rise = esp_timer_get_time();
	//tickVals->tick_fall =-1;

	//esp_rom_printf("rise_handler\n");
}

static void IRAM_ATTR fall_handler(void *args){
	//esp_rom_printf("fall_handler\n");
	pwmEvent_t *tickVals = (pwmEvent_t *)args;

	//if(tickVals->tick_rise==-1){
		
		tickVals->tick_fall = esp_timer_get_time();

		// if((abs((tickVals->tick_fall-tickVals->tick_rise)-tickVals->dTime)>15)&&((tickVals->tick_fall-tickVals->tick_rise)>2)){
		if ((tickVals->tick_fall - tickVals->tick_rise) > 2){
			tickVals->flag = 1;
			tickVals->dTime = (tickVals->tick_fall - tickVals->tick_rise);
		}
	//}
}


void encoderPPM_task(void *arg)
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
	if (strstr(me_config.slot_options[slot_num], "floatOutput") != NULL){
		float_output = 1;
		ESP_LOGD(TAG, "float_output mode: %d", slot_num);
	}

	uint8_t dirInverse = 0;
	if (strstr(me_config.slot_options[slot_num], "dirInverse") != NULL){
		dirInverse = 1;
		ESP_LOGD(TAG, "dirInverse slot: %d", slot_num);
	}

	uint8_t zero_shift = 0;
	if (strstr(me_config.slot_options[slot_num], "zeroShift") != NULL){
		zero_shift = get_option_int_val(slot_num, "zeroShift");
		ESP_LOGD(TAG, "zero_shift: %d", zero_shift);
	}

	int8_t offset = 0;
	if (strstr(me_config.slot_options[slot_num], "offset") != NULL){
		offset = get_option_int_val(slot_num, "offset");
		ESP_LOGD(TAG, "zero_shift: %d", zero_shift);
	}

	uint16_t refreshPeriod = 1000/50;
    if (strstr(me_config.slot_options[slot_num], "refreshRate") != NULL) {
		refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate"));
		ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",refreshPeriod, slot_num);
	}

	uint16_t MIN_VAL = 5;
	if (strstr(me_config.slot_options[slot_num], "ppmMinVal") != NULL){
		MIN_VAL = get_option_int_val(slot_num, "ppmMinVal");
		ESP_LOGD(TAG, "ppmMinVal: %d", MIN_VAL);
	}

	uint16_t MAX_VAL = 900;
	if (strstr(me_config.slot_options[slot_num], "ppmMaxVal") != NULL){
		MAX_VAL = get_option_int_val(slot_num, "ppmMaxVal");
		ESP_LOGD(TAG, "ppmMaxVal: %d", MAX_VAL);
	}

	uint16_t deadZone = 0;
	if (strstr(me_config.slot_options[slot_num], "deadZone") != NULL){
		deadZone = get_option_int_val(slot_num, "deadZone");
		ESP_LOGD(TAG, "deadZone: %d", deadZone);
	}

	float filterK = 1.0;
	if (strstr(me_config.slot_options[slot_num], "filterK") != NULL){
		filterK = get_option_float_val(slot_num, "filterK");
		ESP_LOGD(TAG, "filterK: %f", filterK);
	}

	uint8_t calibrationFlag = 0;
	if (strstr(me_config.slot_options[slot_num], "calibration") != NULL){
		calibrationFlag = 1;
		ESP_LOGD(TAG, "calibrationFlag!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
	}

	
	int num_of_pos;
	if (strstr(me_config.slot_options[slot_num], "numOfPos") != NULL)	{
		num_of_pos = get_option_int_val(slot_num, "numOfPos");
		if (num_of_pos <= 0){
			ESP_LOGD(TAG, "pwmEncoder num_of_pos wrong format, set default slot:%d", slot_num);
			num_of_pos = 24; // default val
		}
	}else{
		num_of_pos = 24; // default val
	}
	ESP_LOGD(TAG, "pwmEncoder num_of_pos:%d slot:%d", num_of_pos, slot_num);
	
	int pole = MAX_VAL - MIN_VAL;
	float pos_length = (float)pole / num_of_pos;
	int16_t raw_val;
	float prew_filtredVal=-1;
	int current_pos, prev_pos = -1;

	if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
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
	while(current_pos>num_of_pos){
		current_pos -= num_of_pos;
	}

	ESP_LOGD(TAG, "pwmEncoder first_val:%d offset:%d pos_legth:%f", raw_val, offset, pos_length);

	float filtredVal = 0;
	TickType_t lastWakeTime = xTaskGetTickCount(); 
	waitForWorkPermit(slot_num);

	while (1){
		//vTaskDelay(pdMS_TO_TICKS(20));
		if (tickVals.flag){
			raw_val = tickVals.dTime + offset;
			//raw_val = tickVals.dTime + offset;
		}else if((esp_timer_get_time()-tickVals.tick_rise)>1000){
			raw_val = 0;
		}

		if(raw_val > pole){
			raw_val -= pole;
		}else if(raw_val < 0){
			raw_val
		}

		if(raw_val<MIN_VAL){
			MIN_VAL = raw_val;
			pole = MAX_VAL - MIN_VAL;
			pos_length = (float)pole / num_of_pos;
		}else if(raw_val>MAX_VAL){
			MAX_VAL = raw_val;
			pole = MAX_VAL - MIN_VAL;
			pos_length = (float)pole / num_of_pos;
		}
		//raw_val = raw_val + offset;
		//ESP_LOGD(TAG, "raw_val:%d", raw_val);
		
		if((filterK<1)&&(abs(raw_val-prew_filtredVal)<pole/2)){
			filtredVal = filtredVal*(1-filterK) + raw_val*(filterK);
			//ESP_LOGD(TAG, "filtredVal:%f raw_val:%d", filtredVal, raw_val);
		}else{
			filtredVal = raw_val;
		}

		if(abs(filtredVal-prew_filtredVal)>deadZone){
			prew_filtredVal = filtredVal;
		
		

		// val_mass[anti_deb_mass_index] = raw_val;
		// anti_deb_mass_index++;
		// if (anti_deb_mass_index == ANTI_DEBOUNCE_INERATIONS){
		// 	anti_deb_mass_index = 0;
		// }
		// int sum = 0;
		// for (int i = 1; i < ANTI_DEBOUNCE_INERATIONS; i++){
		// 	if (abs(val_mass[i] - val_mass[i - 1]) < 3)	{
		// 		sum++;
		// 	}
		// }
		// if (sum >= (ANTI_DEBOUNCE_INERATIONS - 1)){
			current_pos = 0;
			float tmpVal = filtredVal;
			while(tmpVal > pos_length){
				current_pos++;
				tmpVal -= pos_length;
			}
			//ESP_LOGD(TAG, "obrezok: %f", tmpVal);
			//current_pos = ((raw_val- pos_length/2)/ pos_length)+zero_shift;
			while(current_pos>=num_of_pos){
				current_pos -= num_of_pos;
			}

			if(dirInverse){
				current_pos = num_of_pos-current_pos;
			}
		}
			// }

		//ESP_LOGD(TAG, "raw_val:%d center:%d delta:%d", raw_val, (int)(current_pos*pos_length+pos_length/2), abs(raw_val-(current_pos*pos_length+pos_length/2)) );

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

		if(calibrationFlag){
			sprintf(str, "/calibration: dTime:%lld pos:%d delta:%d", tickVals.dTime, current_pos, abs(raw_val-(current_pos*pos_length+pos_length/2)));
			ESP_LOGD(TAG,"%s", str);
			report(str, slot_num);
		}
		tickVals.flag = 0;

		if (xTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(refreshPeriod)) == pdFALSE) {
            ESP_LOGE(TAG, "Delay missed! Adjusting wake time.");
            lastWakeTime = xTaskGetTickCount(); // Сброс времени пробуждения
        }
		
	}
}

void start_encoderPPM_task(int slot_num)
{

	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	// int slot_num = *(int*) arg;
	xTaskCreate(encoderPPM_task, "encoderCalc", 1024 * 4, &t_slot_num, 22, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "pwmEncoder init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

void encoder_inc_task(void *arg){
    int slot_num = *(int *)arg;

	me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    uint8_t a_pin_num = SLOTS_PIN_MAP[slot_num][0];
    uint8_t b_pin_num = SLOTS_PIN_MAP[slot_num][1];

	uint8_t inverse  = 0;
    if (strstr(me_config.slot_options[slot_num], "inverse")!=NULL){
        inverse=1;
		ESP_LOGD(TAG, "Set inverse slot_num: %d",slot_num);
    }

    //ESP_LOGD(TAG, "encoder_inc_task slot_num: %d, a_pin_num: %d, b_pin_num: %d", slot_num, a_pin_num, b_pin_num);

    pcnt_unit_config_t unit_config = {
        .high_limit = INT16_MAX,
        .low_limit = INT16_MIN,
    };
    pcnt_unit_handle_t pcnt_unit = NULL;
    esp_err_t err = pcnt_new_unit(&unit_config, &pcnt_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pcnt_new_unit failed: %d", err);
        return;
    }

	//ESP_LOGI(TAG, "set glitch filter");
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 1000,
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
	if(!inverse){
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
	ESP_LOGI(TAG, "enable pcnt unit");
    ESP_ERROR_CHECK(pcnt_unit_enable(pcnt_unit));
    ESP_LOGI(TAG, "clear pcnt unit");
    ESP_ERROR_CHECK(pcnt_unit_clear_count(pcnt_unit));
    ESP_LOGI(TAG, "start pcnt unit");
    ESP_ERROR_CHECK(pcnt_unit_start(pcnt_unit));


    uint8_t absolute  = 0;
    if (strstr(me_config.slot_options[slot_num], "absolute")!=NULL){
        absolute=1;
		ESP_LOGD(TAG, "Set absolute for slot:%d", slot_num);
    }

    uint16_t refreshPeriod = 10;
    if (strstr(me_config.slot_options[slot_num], "refreshPeriod") != NULL) {
		refreshPeriod = (get_option_int_val(slot_num, "refreshPeriod"));
		ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",refreshPeriod, slot_num);
	}

	int16_t divider = 1;
    if (strstr(me_config.slot_options[slot_num], "divider") != NULL) {
		divider = (get_option_int_val(slot_num, "divider"));
		if(divider<=0) divider=1;
		ESP_LOGD(TAG, "Set divider:%d for slot:%d",divider, slot_num);
	}
	int16_t offset = 0;
    if (strstr(me_config.slot_options[slot_num], "offset") != NULL) {
		offset = (get_option_int_val(slot_num, "offset"));
		ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",refreshPeriod, slot_num);
	}

	int32_t minVal = INT32_MIN;
    if (strstr(me_config.slot_options[slot_num], "minVal") != NULL) {
		minVal = (get_option_int_val(slot_num, "minVal"));
		ESP_LOGD(TAG, "Set minVal:%ld for slot:%d",minVal, slot_num);
	}
	int32_t maxVal = INT32_MAX;
    if (strstr(me_config.slot_options[slot_num], "maxVal") != NULL) {
		maxVal = (get_option_int_val(slot_num, "maxVal"));
		ESP_LOGD(TAG, "Set maxVal:%ld for slot:%d",maxVal, slot_num);
	}

	int32_t range = maxVal - minVal+1;
	ESP_LOGD(TAG, "Set encoder range:%ld for slot:%d",range, slot_num);

	

    //---
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
        me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "stepper_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/encoder_")+3];
		sprintf(t_str, "%s/encoder_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
        me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart encoder_topic:%s", me_state.action_topic_list[slot_num]);
	}

    int32_t rawCount = 0;
    int32_t count = 0;
	int32_t prev_count = -1;

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

        err = pcnt_unit_get_count(pcnt_unit, &rawCount);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pcnt_get_counter_value failed: %d", err);
            return;
		}
		count = (rawCount+offset)/divider;
		while(count<minVal){
			count=count+range;
		}
		while(count>maxVal){
			count=count-range;
		}

        if (count != prev_count) {
            char str[40];

            if (absolute) {
				
                sprintf(str, "%ld", count);
            } else {
				int32_t diff = count - prev_count;
                sprintf(str, "%ld", diff);
            }

            report(str, slot_num);
            prev_count = count;
        }

        vTaskDelayUntil(&lastWakeTime, refreshPeriod);
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


// Функция для чтения 8-битного регистра AS5600
static esp_err_t as5600_read_register_8bit(i2c_port_t i2c_port, uint8_t as5600_addr, uint8_t reg_addr, uint8_t *data)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (as5600_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (as5600_addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, data, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(i2c_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

// Функция для чтения 16-битного регистра AS5600
static esp_err_t as5600_read_register_16bit(i2c_port_t i2c_port, uint8_t as5600_addr, uint8_t reg_addr_high, uint8_t reg_addr_low, uint16_t *data)
{
    uint8_t high_byte, low_byte;
    esp_err_t ret = as5600_read_register_8bit(i2c_port, as5600_addr, reg_addr_high, &high_byte);
    if (ret != ESP_OK) return ret;
    
    ret = as5600_read_register_8bit(i2c_port, as5600_addr, reg_addr_low, &low_byte);
    if (ret != ESP_OK) return ret;
    
    *data = ((uint16_t)high_byte << 8) | low_byte;
    return ESP_OK;
}

void encoderAS5600_task(void *arg)
{
    int slot_num = *(int *)arg;
    
    char str[512];
    
    // Настройка I2C
    uint8_t i2c_sda_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t i2c_scl_pin = SLOTS_PIN_MAP[slot_num][1];
    
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = i2c_sda_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = i2c_scl_pin,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    
    i2c_port_t i2c_port = I2C_NUM_0 + slot_num;
    esp_err_t ret = i2c_param_config(i2c_port, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed for slot %d", slot_num);
        return;
    }
    
    ret = i2c_driver_install(i2c_port, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed for slot %d", slot_num);
        return;
    }
    
    // Адрес AS5600
    uint8_t as5600_addr = 0x36;
    
    // Парсинг опций - только период обновления
    uint16_t refreshPeriod = 100; // по умолчанию 100мс
    if (strstr(me_config.slot_options[slot_num], "refreshPeriod") != NULL) {
        refreshPeriod = get_option_int_val(slot_num, "refreshPeriod");
        ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d", refreshPeriod, slot_num);
    }
    
    // Настройка топика
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
        char* custom_topic = NULL;
        custom_topic = get_option_string_val(slot_num, "topic");
        me_state.trigger_topic_list[slot_num] = strdup(custom_topic);
        ESP_LOGD(TAG, "Custom topic:%s", me_state.trigger_topic_list[slot_num]);
    } else {
        char t_str[strlen(me_config.deviceName) + strlen("/as5600_") + 3];
        sprintf(t_str, "%s/as5600_%d", me_config.deviceName, slot_num);
        me_state.trigger_topic_list[slot_num] = strdup(t_str);
        ESP_LOGD(TAG, "Standard topic:%s", me_state.trigger_topic_list[slot_num]);
    }
    
    // Проверка связи с AS5600
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (as5600_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(i2c_port, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AS5600 not found on slot %d", slot_num);
        i2c_driver_delete(i2c_port);
        return;
    }
    
    ESP_LOGD(TAG, "AS5600 found on slot %d", slot_num);
    
    uint16_t current_angle = 0;
    uint16_t current_raw_angle = 0;
    uint16_t current_magnitude = 0;
    uint8_t current_agc = 0;
    
    uint16_t prev_angle = 0xFFFF;
    uint16_t prev_raw_angle = 0xFFFF;
    uint16_t prev_magnitude = 0xFFFF;
    uint8_t prev_agc = 0xFF;
    
    bool first_read = true;
    
    TickType_t lastWakeTime = xTaskGetTickCount();
    
    waitForWorkPermit(slot_num);
    
    while (1) {
        bool data_changed = false;
        
        // Чтение компенсированного угла (ANGLE регистр 0x0E, 0x0F)
        ret = as5600_read_register_16bit(i2c_port, as5600_addr, 0x0E, 0x0F, &current_angle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read ANGLE register, slot %d", slot_num);
            goto next_iteration;
        }
        current_angle = current_angle & 0x0FFF; // Маскируем только 12 бит
        
        // Чтение сырого угла (RAW ANGLE регистр 0x0C, 0x0D)
        ret = as5600_read_register_16bit(i2c_port, as5600_addr, 0x0C, 0x0D, &current_raw_angle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read RAW ANGLE register, slot %d", slot_num);
            goto next_iteration;
        }
        current_raw_angle = current_raw_angle & 0x0FFF; // Маскируем только 12 бит
        
        // Чтение AGC (регистр 0x1A)
        ret = as5600_read_register_8bit(i2c_port, as5600_addr, 0x1A, &current_agc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read AGC register, slot %d", slot_num);
            goto next_iteration;
        }
        
        // Чтение MAGNITUDE (регистр 0x1B, 0x1C)
        ret = as5600_read_register_16bit(i2c_port, as5600_addr, 0x1B, 0x1C, &current_magnitude);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read MAGNITUDE register, slot %d", slot_num);
            goto next_iteration;
        }
        current_magnitude = current_magnitude & 0x0FFF; // Маскируем только 12 бит
        
        // Проверяем изменения
        if (first_read || 
            current_angle != prev_angle || 
            current_raw_angle != prev_raw_angle || 
            current_agc != prev_agc || 
            current_magnitude != prev_magnitude) {
            
            data_changed = true;
        }
        
        if (data_changed) {
            // Преобразуем углы в градусы (0-4095 -> 0-360°)
            float angle_degrees = (float)current_angle * 360.0f / 4095.0f;
            float raw_angle_degrees = (float)current_raw_angle * 360.0f / 4095.0f;
            
            // Формируем JSON-строку с полной информацией
            sprintf(str, "{\"angle\":%.2f,\"raw_angle\":%.2f,\"agc\":%d,\"magnitude\":%d}", 
                   angle_degrees, raw_angle_degrees, current_agc, current_magnitude);
            
            report(str, slot_num);
            
            ESP_LOGD(TAG, "AS5600 slot %d: angle=%.2f°, raw_angle=%.2f°, agc=%d, magnitude=%d", 
                    slot_num, angle_degrees, raw_angle_degrees, current_agc, current_magnitude);
            
            // Обновляем предыдущие значения
            prev_angle = current_angle;
            prev_raw_angle = current_raw_angle;
            prev_agc = current_agc;
            prev_magnitude = current_magnitude;
            first_read = false;
        }
        
        next_iteration:
        vTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(refreshPeriod));
    }
    
    // Очистка ресурсов (никогда не достигается в бесконечном цикле)
    i2c_driver_delete(i2c_port);
}
void start_encoderAS5600_task(int slot_num)
{
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
    
    xTaskCreate(encoderAS5600_task, "encoderAS5600_task", 1024 * 4, &t_slot_num, 5, NULL);
    
    ESP_LOGD(TAG, "encoderAS5600_task init ok: %d Heap usage: %lu free heap:%u", 
             slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}