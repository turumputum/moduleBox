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
} ADC1_CHANNEL, * PADC1_CHANNEL; 


// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

//extern uint8_t 				SLOTS_PIN_MAP		[10][4];
extern configuration 		me_config;
extern 	stateStruct 		me_state;
extern uint8_t 				led_segment;



static const char *			TAG 				= "ANALOG";

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
static adc_channel_t SLOT_ADC_MAP[ NUM_OF_SLOTS ] =
{
    ADC1_CHANNEL_3,
    -1,
    -1,
    ADC1_CHANNEL_2,
    ADC1_CHANNEL_1,
    ADC1_CHANNEL_6
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
static void adc1_start()
{
	if ((adc1_handle != NULL) && !started)
	{
		if( xSemaphoreTake(startSemaphore, portMAX_DELAY) == pdTRUE)
		{
			if (!started && (adc_continuous_start(adc1_handle) == ESP_OK))
			{
				adc1_dig_cfg.adc_pattern = adc1_pattern;
				ESP_ERROR_CHECK(adc_continuous_config(adc1_handle, &adc1_dig_cfg));

				started = true;
			}

			xSemaphoreGive(startSemaphore);
		}
	}
}
static bool adc1_add_channel(int slot_num)
{
	bool result  	= false;
	PADC1_CHANNEL	ch = &adc1_channels[slot_num];

	if ((SLOT_ADC_MAP[slot_num] != -1) && (!ch->used))
	{
		ch->s_task_handle = xTaskGetCurrentTaskHandle();
		ch->cb.on_conv_done = s_conv_done_cb;
		ch->pattern_num	= adc1_dig_cfg.pattern_num;

		adc_digi_pattern_config_t * p = &adc1_pattern[ch->pattern_num];

		p->atten 		= ADC_ATTEN_DB_12;
		p->channel 		= SLOT_ADC_MAP[slot_num];
		p->unit 		= ADC_UNIT_1;
		p->bit_width 	= SOC_ADC_DIGI_MAX_BITWIDTH;

		if (adc_continuous_register_event_callbacks(adc1_handle, &ch->cb, NULL) == ESP_OK)
		{
			result = true;			

			adc1_dig_cfg.pattern_num++;
			ch->used = true;
		}
	}

	return result;
}
void adc1_task(void *arg)
{
    uint32_t ret_num = 0;
    int slot_num = *(int *)arg;
	uint8_t result[ EXAMPLE_READ_LEN ] = {0};

//--------------------------------------------------

    uint16_t MIN_VAL = 0;
    uint16_t MAX_VAL = 4095;
    uint8_t flag_float_output=0;
    if (strstr(me_config.slot_options[slot_num], "floatOutput")!=NULL){
		flag_float_output = 1;
		ESP_LOGD(TAG, "Set float output. Slot:%d", slot_num);
	}
	if (strstr(me_config.slot_options[slot_num], "maxVal")!=NULL){
		MAX_VAL = get_option_int_val(slot_num, "maxVal");
		ESP_LOGD(TAG, "Set max_val:%d. Slot:%d", MAX_VAL, slot_num);
	}
    if (strstr(me_config.slot_options[slot_num], "minVal")!=NULL){
		MIN_VAL = get_option_int_val(slot_num, "minVal");
		ESP_LOGD(TAG, "Set min_val:%d. Slot:%d", MIN_VAL, slot_num);
	}

    uint8_t inverse  = 0;
	if (strstr(me_config.slot_options[slot_num], "inverse")!=NULL){
		inverse=1;
	}

    float k=1;
    if (strstr(me_config.slot_options[slot_num], "filterK")!=NULL){
        k = get_option_float_val(slot_num, "filterK");
		ESP_LOGD(TAG, "Set k filter:%f.  Slot:%d", k, slot_num);
	}
    
    uint16_t dead_band=10;
    if (strstr(me_config.slot_options[slot_num], "deadBand")!=NULL){
        dead_band = get_option_int_val(slot_num, "deadBand");
		ESP_LOGD(TAG, "Set dead_band:%d. Slot:%d",dead_band, slot_num);
	}

	uint16_t periodic=0;
    if (strstr(me_config.slot_options[slot_num], "periodic")!=NULL){
        periodic = get_option_int_val(slot_num, "periodic");
		ESP_LOGD(TAG, "Set periodic:%d. Slot:%d",periodic, slot_num);
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

	if (adc1_add_channel(slot_num))
	{
		waitForWorkPermit(slot_num);

		adc1_start();

		TickType_t lastWakeTime = xTaskGetTickCount();

		while (1) 
		{
			ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

#define EXAMPLE_ADC_UNIT                    ADC_UNIT_1
#define _EXAMPLE_ADC_UNIT_STR(unit)         #unit

			char unit[] = EXAMPLE_ADC_UNIT_STR(EXAMPLE_ADC_UNIT);

			while (adc_continuous_read(adc1_handle, result, EXAMPLE_READ_LEN, &ret_num, 0) == ESP_OK)
			{
				ESP_LOGI("TASK", "ret_num is %"PRIu32" bytes", ret_num);
				for (int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES) {
					adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];
					uint32_t chan_num = EXAMPLE_ADC_GET_CHANNEL(p);
					uint32_t data = EXAMPLE_ADC_GET_DATA(p);
					/* Check the channel number validation, the data is invalid if the channel num exceed the maximum channel */
					if (chan_num < SOC_ADC_CHANNEL_NUM(EXAMPLE_ADC_UNIT)) {
						ESP_LOGI(TAG, "Unit: %s, Channel: %"PRIu32", Value: %"PRIx32, unit, chan_num, data);
					} else {
						ESP_LOGW(TAG, "Invalid data [%s_%"PRIu32"_%"PRIx32"]", unit, chan_num, data);
					}
				}
				/**
				 * Because printing is slow, so every time you call `ulTaskNotifyTake`, it will immediately return.
				 * To avoid a task watchdog timeout, add a delay here. When you replace the way you process the data,
				 * usually you don't need this delay (as this task will block for a while).
				 */
				vTaskDelay(1);
			}
		}

		ESP_ERROR_CHECK(adc_continuous_deinit(adc1_handle));
	}
}
void start_adc1_task(int slot_num)
{
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;

	xTaskCreatePinnedToCore(adc1_task, "adc1_task", 1024 * 4, &t_slot_num, 12, NULL,1);

	ESP_LOGD(TAG, "adc1_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
