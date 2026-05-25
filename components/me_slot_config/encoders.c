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
	Использует PCNT периферию (ESP32-S3 имеет всего 4 PCNT unit'а суммарно
	на encoderInc + tachometer + stepper).
	slots: 0-3
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

	/* Делитель импульсов энкодера (на сколько импульсов = 1 шаг позиции)
	   По умолчанию 1 (без делителя)
	*/
	c->divider = get_option_int_val(slot_num, "divider", "", 1, 1, UINT16_MAX);
	ESP_LOGD(TAG, "Encoder slotNum:%d divider:%d", slot_num, c->divider);


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
    stdcommand_register(&c->cmds, INCCMD_reset, "action/reset", PARAMT_none);

	/* Рапортует текущее значение
	*/
	c->report = stdreport_register(RPTT_string, slot_num, "", "event/val");
}

void encoder_inc_task(void *arg){
    int slot_num = (int)(intptr_t)arg;

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
        ESP_LOGW(TAG, "PCNT unit limit reached (slot:%d), task terminated. err:%d",
                 slot_num, err);
        vTaskDelete(NULL);
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

    int rawVal = 0; //pcnt count - pcnt_unit_get_count expects int*
    int32_t prewRawVal=0; //pcnt coint in int16 MAX-MIN range
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

			stdreport_s(c.report, str);
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
	// int slot_num = (int)(intptr_t)arg;
	xTaskCreate(encoder_inc_task, "encoder_inc_task", 1024 * 4, (void*)(intptr_t)slot_num, 1, NULL);
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
	Модуль для работы с энкодером AS5600 по I2C, абсолютный энкодер с разрешающей способностью 12 бит, 4096 позиций на окружности.
	slots: 0-5
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

	/* Значение фильтра, плавающее среднее. Чем ниже значение, тем сильнее фильтрация
		- диапазон от 0.0 до 1.0, где 1.0 - без фильтрации
	*/
	c->filterK = get_option_float_val(slot_num, "filterK", 1.0);
	ESP_LOGD(TAG, "[encoder_%d] filterK: %f", slot_num, c->filterK);

	c->minVal = 0;
	c->maxVal = 4095;
	c->pole = c->maxVal - c->minVal;

	/* Количество сегментов на окружности
	*/
	c->numOfPos = get_option_int_val(slot_num, "numOfPos", "", 24, 2, 4095);
	ESP_LOGD(TAG, "[encoder_%d] numOfPos:%d ", slot_num, c->numOfPos);

	
	/* Период проверки значений
		- еденицы измерения раз в секунду
	*/
	c->refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate","fps", 20, 1, 100));
	ESP_LOGD(TAG, "[encoder_%d] refreshPeriod:%d", slot_num,c->refreshPeriod);
	
	
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
	c->absReport = stdreport_register(RPTT_string, slot_num, "", "event/abs");

	/* Рапортует текущее инкрементальное приращение
	*/
	c->incReport = stdreport_register(RPTT_string, slot_num, "", "event/inc");

	/* Рапортует текущее отношение в формате числа с плавающей точкой
	*/
	c->floatReport = stdreport_register(RPTT_string, slot_num, "", "event/float");

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
		ESP_LOGW(TAG, "%s", tmpStr);
		mblog(W, tmpStr);
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
	int slot_num = (int)(intptr_t)arg;
	
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
		ESP_LOGW(TAG, "%s", tmpStr);
		mblog(W, tmpStr);
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
		// ESP_LOGD(TAG, "rawVal:%d angle on slot:%d", raw_angle, slot_num);
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
	
	xTaskCreate(encoderAS5600_task, "encoderAS5600", 1024 * 4, (void*)(intptr_t)slot_num, 1, NULL);
	
	ESP_LOGD(TAG, "AS5600 encoder init ok: %d Heap usage: %lu free heap:%u", 
			 slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}