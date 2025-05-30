// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/timer.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"
#include <driver/mcpwm.h>
#include "stateConfig.h"
#include "me_slot_config.h"
#include "reporter.h"

#include "esp_adc/adc_continuous.h"

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define LOG_LOCAL_LEVEL 	ESP_LOG_DEBUG

#define MODE_3V3 			0
#define MODE_5V 			1
#define MODE_10V 			2
#define EXAMPLE_READ_LEN    (64 * SOC_ADC_DIGI_DATA_BYTES_PER_CONV)

#define EXAMPLE_ADC_UNIT                    ADC_UNIT_1
#define _EXAMPLE_ADC_UNIT_STR(unit)         #unit
#define EXAMPLE_ADC_UNIT_STR(unit)          _EXAMPLE_ADC_UNIT_STR(unit)
#define EXAMPLE_ADC_CONV_MODE               ADC_CONV_SINGLE_UNIT_1
#define EXAMPLE_ADC_ATTEN                   ADC_ATTEN_DB_0
#define EXAMPLE_ADC_BIT_WIDTH               SOC_ADC_DIGI_MAX_BITWIDTH

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
#define EXAMPLE_ADC_OUTPUT_TYPE             ADC_DIGI_OUTPUT_FORMAT_TYPE1
#define EXAMPLE_ADC_GET_CHANNEL(p_data)     ((p_data)->type1.channel)
#define EXAMPLE_ADC_GET_DATA(p_data)        ((p_data)->type1.data)
#else
#define EXAMPLE_ADC_OUTPUT_TYPE             ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define EXAMPLE_ADC_GET_CHANNEL(p_data)     ((p_data)->type2.channel)
#define EXAMPLE_ADC_GET_DATA(p_data)        ((p_data)->type2.data)
#endif




// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_ADC1_CHANNEL
{
	bool 					used;
	int 					pattern_num;
	TaskHandle_t			s_task_handle;
	adc_continuous_evt_cbs_t cb;
	int 					slot_num;

    uint16_t 				MIN_VAL;
    uint16_t 				MAX_VAL;
    uint8_t 				flag_float_output;
	uint8_t 				inverse;
	float 					k;
	uint16_t 				dead_band;
	uint16_t 				periodic;

} ADC1_CHANNEL, * PADC1_CHANNEL; 


// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

//extern uint8_t 				SLOTS_PIN_MAP		[10][4];
extern configuration 		me_config;
extern 	stateStruct 		me_state;
extern uint8_t 				led_segment;



static const char *			TAG 				= "ADC1";

static xSemaphoreHandle 	startSemaphore		= NULL; 

static adc_continuous_handle_cfg_t adc1_config = 
		{
			.max_store_buf_size = 1024,
			.conv_frame_size = EXAMPLE_READ_LEN
		};


adc_continuous_handle_t		adc1_handle			= NULL;
static bool 				started 			= false;

static 
adc_continuous_config_t 	adc1_dig_cfg = 
	{
		.sample_freq_hz 	= 20 * 1000,
		.conv_mode 			= ADC_CONV_SINGLE_UNIT_1,
		.format 			= ADC_DIGI_OUTPUT_FORMAT_TYPE2,
	};


static 
adc_digi_pattern_config_t 	adc1_pattern		[ SOC_ADC_PATT_LEN_MAX ] 	= { 0 };
static ADC1_CHANNEL 		adc1_channels 		[ NUM_OF_SLOTS ] 			= { 0 };

extern uint8_t SLOTS_PIN_MAP[10][4];
static adc_channel_t CHANNEL_ADC1_MAP[ NUM_OF_SLOTS ] =
{
    ADC1_CHANNEL_3, // SLOT 0
    -1,				// SLOT 1
    -1,				// SLOT 2
    ADC1_CHANNEL_2,	// SLOT 3
    ADC1_CHANNEL_1,	// SLOT 4
    ADC1_CHANNEL_6	// SLOT 5
};
static int SLOT_ADC1_MAP[ NUM_OF_SLOTS ] =
{
    -1,				// ADC1_CHANNEL_0 -> none
    4,				// ADC1_CHANNEL_1 -> SLOT 4
    3,				// ADC1_CHANNEL_2 -> SLOT 3
    0, 				// ADC1_CHANNEL_3 -> SLOT 0 
    -1,				// ADC1_CHANNEL_4 -> none
    -1,				// ADC1_CHANNEL_5 -> none
	5,  			// ADC1_CHANNEL_6 -> SLOT 5
};


// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

static bool IRAM_ATTR s_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    BaseType_t mustYield = pdFALSE;
	PADC1_CHANNEL  ch = (PADC1_CHANNEL)user_data;
    //Notify that ADC continuous driver has done enough number of conversions
    vTaskNotifyGiveFromISR(ch->s_task_handle, &mustYield);

    return (mustYield == pdTRUE);
}

static bool adc1_init()
{
	bool result = true;

	if (adc1_handle == NULL)
	{
		ESP_ERROR_CHECK(adc_continuous_new_handle(&adc1_config, &adc1_handle));

		if (adc1_handle != NULL)
		{
			startSemaphore = xSemaphoreCreateMutex();
		}
		else
			result = false;
	}

	return result;
}
static bool adc1_start_and_there_can_be_only_one()
{
	bool result = false;

	if ((adc1_handle != NULL) && !started)
	{
		if( xSemaphoreTake(startSemaphore, portMAX_DELAY) == pdTRUE)
		{
			if (!started)
			{
				adc1_dig_cfg.adc_pattern = adc1_pattern;

				if (adc_continuous_config(adc1_handle, &adc1_dig_cfg) == ESP_OK)
				{
					if (adc_continuous_start(adc1_handle) == ESP_OK)
					{
						result = started = true;
					}
				}
			}

			xSemaphoreGive(startSemaphore);
		}
	}

	return result;
}
static bool adc1_add_channel(PADC1_CHANNEL	ch)
{
	bool result  	= false;

	if ((CHANNEL_ADC1_MAP[ch->slot_num] != -1) && (!ch->used))
	{
		ch->s_task_handle = xTaskGetCurrentTaskHandle();
		ch->cb.on_conv_done = s_conv_done_cb;
		ch->pattern_num	= adc1_dig_cfg.pattern_num;

		adc_digi_pattern_config_t * p = &adc1_pattern[ch->pattern_num];

		p->atten 		= ADC_ATTEN_DB_12;
		p->channel 		= CHANNEL_ADC1_MAP[ch->slot_num];
		p->unit 		= ADC_UNIT_1;
		p->bit_width 	= SOC_ADC_DIGI_MAX_BITWIDTH;

		// Добавляем CB только для первого потока, он и будет рабочим.
		if (!adc1_dig_cfg.pattern_num)
		{
			ESP_LOGD(TAG, "ADC CB was set on slot %d", ch->slot_num);

			if (adc_continuous_register_event_callbacks(adc1_handle, &ch->cb, ch) == ESP_OK)
			{
				result = true;			
			}
		}
		else	
			result = true;

		if (result)
		{
			adc1_dig_cfg.pattern_num++;
			ch->used = true;
		}
	}

	return result;
}
void adc1_task(void *arg)
{
    uint32_t 		ret_num 	= 0;
    int 			slot_num 	= *(int *)arg;
	PADC1_CHANNEL	ch 			= &adc1_channels[slot_num];
	uint8_t 		result		[ EXAMPLE_READ_LEN ] = {0};


	ch->slot_num 			= slot_num;
    ch->MIN_VAL 			= 0;
    ch->MAX_VAL 			= 4095;
    ch->flag_float_output	= 0;
	ch->inverse  			= 0;
	ch->k					= 1;
	ch->dead_band			= 10;
	ch->periodic			= 0;

    if (strstr(me_config.slot_options[slot_num], "floatOutput")!=NULL){
		ch->flag_float_output = 1;
		ESP_LOGD(TAG, "Set float output. Slot:%d", slot_num);
	}
	if (strstr(me_config.slot_options[slot_num], "maxVal")!=NULL){
		ch->MAX_VAL = get_option_int_val(slot_num, "maxVal");
		ESP_LOGD(TAG, "Set max_val:%d. Slot:%d", ch->MAX_VAL, slot_num);
	}
    if (strstr(me_config.slot_options[slot_num], "minVal")!=NULL){
		ch->MIN_VAL = get_option_int_val(slot_num, "minVal");
		ESP_LOGD(TAG, "Set min_val:%d. Slot:%d", ch->MIN_VAL, slot_num);
	}
	if (strstr(me_config.slot_options[slot_num], "inverse")!=NULL){
		ch->inverse = 1;
	}
   
    if (strstr(me_config.slot_options[slot_num], "filterK")!=NULL){
        ch->k = get_option_float_val(slot_num, "filterK");
		ESP_LOGD(TAG, "Set k filter:%f.  Slot:%d", ch->k, slot_num);
	}
    
    if (strstr(me_config.slot_options[slot_num], "deadBand")!=NULL){
        ch->dead_band = get_option_int_val(slot_num, "deadBand");
		ESP_LOGD(TAG, "Set dead_band:%d. Slot:%d", ch->dead_band, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "periodic")!=NULL){
        ch->periodic = get_option_int_val(slot_num, "periodic");
		ESP_LOGD(TAG, "Set periodic:%d. Slot:%d", ch->periodic, slot_num);
	}

	uint8_t divPin_1 = SLOTS_PIN_MAP[slot_num][2];
	esp_rom_gpio_pad_select_gpio(divPin_1);
	gpio_set_direction(divPin_1, GPIO_MODE_OUTPUT);
	uint8_t divPin_2 = SLOTS_PIN_MAP[slot_num][1];
	esp_rom_gpio_pad_select_gpio(divPin_2);
	gpio_set_direction(divPin_2, GPIO_MODE_OUTPUT);

	gpio_set_level(divPin_1, 1);
	gpio_set_level(divPin_2, 0);
	ESP_LOGD(TAG, "Set dividerMode:5V. Slot:%d", slot_num);

    if (strstr(me_config.slot_options[slot_num], "dividerMode")!=NULL){
        char *dividerModeStr = get_option_string_val(slot_num, "dividerMode");
		if(strcmp(dividerModeStr, "3V3")==0){
			gpio_set_level(divPin_1, 0);
			gpio_set_level(divPin_2, 0);
			ESP_LOGD(TAG, "Set dividerMode:3V3. Slot:%d", slot_num);
		}else if(strcmp(dividerModeStr, "10V")==0){
			gpio_set_level(divPin_1, 0);
			gpio_set_level(divPin_2, 1);
			ESP_LOGD(TAG, "Set dividerMode:10V. Slot:%d", slot_num);
		}
	}

    uint8_t flag_custom_topic = 0;
	char *custom_topic=NULL;
	if (strstr(me_config.slot_options[slot_num], "topic")!=NULL){
		custom_topic = get_option_string_val(slot_num,"topic");
		ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
		flag_custom_topic=1;
	}

    if(flag_custom_topic==0){
		char *str = calloc(strlen(me_config.deviceName)+strlen("/analog_")+4, sizeof(char));
		sprintf(str, "%s/analog_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=str;
	}else{
		me_state.trigger_topic_list[slot_num]=custom_topic;
	}

	uint8_t oversumple = 150;

	uint32_t tmp = 0;
//--------------------------------------------------

	adc1_init();

	if (adc1_add_channel(ch))
	{
		waitForWorkPermit(slot_num);

		if (adc1_start_and_there_can_be_only_one(slot_num))
		{
			ESP_LOGD(TAG, "task on slot %d remained on duty and processes all channels.", slot_num);
			//TickType_t lastWakeTime = xTaskGetTickCount();

			while (1) 
			{
				ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

	#define EXAMPLE_ADC_UNIT                    ADC_UNIT_1
	#define _EXAMPLE_ADC_UNIT_STR(unit)         #unit

				char unit[] = EXAMPLE_ADC_UNIT_STR(EXAMPLE_ADC_UNIT);

				while (adc_continuous_read(adc1_handle, result, EXAMPLE_READ_LEN, &ret_num, 0) == ESP_OK)
				{
					for (int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES)
					{
						adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];
						
						uint32_t chan_num = EXAMPLE_ADC_GET_CHANNEL(p);
						uint32_t data = EXAMPLE_ADC_GET_DATA(p);

						int slot = SLOT_ADC1_MAP[chan_num];

						if (slot >= 0)
						{
							PADC1_CHANNEL	c = &adc1_channels[slot];
							//ESP_LOGD(TAG, "slot %d = %d", slot, (int)data);

						}
					}

					vTaskDelay(1);
				}
			}

			ESP_ERROR_CHECK(adc_continuous_deinit(adc1_handle));
		}
	}
	else
	{
		ESP_LOGD(TAG, "Error adding channel for slot %d", slot_num);	
	}

	ESP_LOGD(TAG, "task of slot %d dismissed", slot_num);

	vTaskDelete(NULL);
}
void start_adc1_task(int slot_num)
{
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;

	xTaskCreatePinnedToCore(adc1_task, "adc1_task", 1024 * 4, &t_slot_num, 12, NULL,1);

	ESP_LOGD(TAG, "adc1_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
