#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/timer.h"
#include "driver/pulse_cnt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"
#include <driver/mcpwm.h>
#include "stateConfig.h"
#include "me_slot_config.h"
#include "reporter.h"


extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern uint8_t led_segment;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "TACHOMETER";

#define BUFFER_SIZE 50  // Default buffer size for RPM averaging

typedef struct {
    uint16_t buffer[BUFFER_SIZE];  // Store pulse counts per second
    uint8_t head;
    uint8_t size;
    uint8_t buffer_size;
} pulse_buffer_t;

typedef struct
{
	pcnt_unit_handle_t pcnt_unit;
	uint64_t last_accumulation_tick;
	uint64_t last_refresh_tick;
	uint16_t rpm;
    uint16_t prev_rpm;
    int last_count;
    uint32_t refresh_period;
    uint16_t divider;
    uint32_t glitch_filter_ns;
    pulse_buffer_t pulse_buffer;
    uint32_t accumulation_time_ms;
} tachoWork_var;

uint64_t debug_flag;



// GPIO interrupt handler removed - using pulse_cnt driver instead

// Initialize pulse buffer
static void pulse_buffer_init(pulse_buffer_t *buffer, uint8_t size) {
    buffer->head = 0;
    buffer->size = 0;
    buffer->buffer_size = (size > BUFFER_SIZE) ? BUFFER_SIZE : size;
    memset(buffer->buffer, 0, sizeof(buffer->buffer));
}

// Add pulse count to buffer
static void pulse_buffer_add(pulse_buffer_t *buffer, uint16_t pulse_count) {
    buffer->buffer[buffer->head] = pulse_count;
    buffer->head = (buffer->head + 1) % buffer->buffer_size;
    if (buffer->size < buffer->buffer_size) {
        buffer->size++;
    }
}

// Calculate RPM from pulse buffer (sum of pulses converted to RPM)
static uint16_t pulse_buffer_get_rpm(pulse_buffer_t *buffer, uint16_t divider, uint32_t accumulation_time_ms) {
    if (buffer->size == 0) {
        return 0;
    }
    
    uint32_t total_pulses = 0;
    for (uint8_t i = 0; i < buffer->size; i++) {
        total_pulses += buffer->buffer[i];
    }
    
    // Convert total pulses to RPM: (total_pulses * 60000) / (accumulation_time_ms * divider)
    // accumulation_time_ms is the total time represented by the buffer
    return (uint16_t)((total_pulses * 60000UL) / (accumulation_time_ms * divider));
}

void tachometer_task(void *arg)
{
	uint16_t resault=0, prev_resault=0;
	uint8_t flag_report=0;

	int slot_num = *(int *)arg;

	uint8_t sens_pin_num = SLOTS_PIN_MAP[slot_num][0];
	
	// Initialize pulse counter
	tachoWork_var var;
	var.refresh_period = 100000; // 100ms in microseconds (default)
	var.last_count = 0;
	uint64_t current_time = esp_timer_get_time();
	var.last_accumulation_tick = current_time;
	var.last_refresh_tick = current_time;
	var.rpm = 0;
	var.prev_rpm = 0;
	var.divider = 1; // Default divider
	var.glitch_filter_ns = 10000;
	var.accumulation_time_ms = 1000; // Default 1 second accumulation time 
	
	// Convert refresh_period to milliseconds for buffer calculations
	uint32_t refresh_period_ms = var.refresh_period / 1000; // Convert microseconds to milliseconds
	
	// Check for refreshRate option (legacy compatibility)
	if (strstr(me_config.slot_options[slot_num], "refreshRate") != NULL) {
		uint16_t refreshRate = get_option_int_val(slot_num, "refreshRate");
		refresh_period_ms = 1000 / refreshRate; // Convert Hz to ms
		var.refresh_period = refresh_period_ms * 1000; // Update refresh_period in microseconds
		ESP_LOGD(TAG, "Set refreshRate:%d Hz, refreshPeriod:%lu ms for slot:%d", refreshRate, refresh_period_ms, slot_num);
	}
	
	// Calculate buffer size: accumulation_time / refresh_period
	uint8_t buffer_size = (var.accumulation_time_ms / refresh_period_ms);
	if (buffer_size > BUFFER_SIZE) buffer_size = BUFFER_SIZE;
	if (buffer_size < 1) buffer_size = 1;
	
	pulse_buffer_init(&var.pulse_buffer, buffer_size);
	
	ESP_LOGD(TAG, "Initial settings - Accumulation time: %lu ms, Refresh period: %lu ms, Buffer size: %d", 
			var.accumulation_time_ms, refresh_period_ms, buffer_size);
	
	
	// Check for pulse divider
	if (strstr(me_config.slot_options[slot_num], "divider")!=NULL){
		uint16_t divider = get_option_int_val(slot_num, "divider");
		if (divider > 0 && divider <= 1000) { // Max 1000 pulses per revolution
			var.divider = divider;
		}
	}
	
	// Check for glitch filter
	if (strstr(me_config.slot_options[slot_num], "glitchFilter")!=NULL){
		uint32_t glitch_ns = get_option_int_val(slot_num, "glitchFilter");
		if (glitch_ns <= 100000) { // Max 100us filter
			var.glitch_filter_ns = glitch_ns;
			ESP_LOGD(TAG, "Set glitch filter to %lu ns", var.glitch_filter_ns);
		}
	}
	
	// Check for accumulation time
	if (strstr(me_config.slot_options[slot_num], "accumulationTime")!=NULL){
		uint32_t accum_time = get_option_int_val(slot_num, "accumulationTime");
		if (accum_time >= 100 && accum_time <= 10000) { // 100ms to 10s
			var.accumulation_time_ms = accum_time;
			// Recalculate buffer size: accumulation_time / refresh_period
			buffer_size = (var.accumulation_time_ms / refresh_period_ms);
			if (buffer_size > BUFFER_SIZE) buffer_size = BUFFER_SIZE;
			if (buffer_size < 1) buffer_size = 1;
			pulse_buffer_init(&var.pulse_buffer, buffer_size);
			ESP_LOGD(TAG, "Set accumulation time to %lu ms, refresh period: %lu ms, buffer size: %d", 
					var.accumulation_time_ms, refresh_period_ms, buffer_size);
		}
	}
	
	pcnt_unit_config_t unit_config = {
		.high_limit = 32767,
		.low_limit = -32768,
		.flags.accum_count = true,
	};
	ESP_ERROR_CHECK(pcnt_new_unit(&unit_config, &var.pcnt_unit));
	
	pcnt_chan_config_t chan_config = {
		.edge_gpio_num = sens_pin_num,
		.level_gpio_num = -1,
		.flags.io_loop_back = false,
	};
	pcnt_channel_handle_t pcnt_chan = NULL;
	ESP_ERROR_CHECK(pcnt_new_channel(var.pcnt_unit, &chan_config, &pcnt_chan));
	
	ESP_ERROR_CHECK(pcnt_channel_set_edge_action(pcnt_chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
	ESP_ERROR_CHECK(pcnt_channel_set_level_action(pcnt_chan, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP));
	
	// Enable glitch filter for debouncing
	if (var.glitch_filter_ns > 0) {
		ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(var.pcnt_unit, &(pcnt_glitch_filter_config_t){
			.max_glitch_ns = var.glitch_filter_ns,
		}));
		ESP_LOGD(TAG, "Glitch filter enabled: %lu ns", var.glitch_filter_ns);
	}
	
	ESP_ERROR_CHECK(pcnt_unit_enable(var.pcnt_unit));
	ESP_ERROR_CHECK(pcnt_unit_clear_count(var.pcnt_unit));
	ESP_ERROR_CHECK(pcnt_unit_start(var.pcnt_unit));

    uint16_t threshold  = 0;
	if (strstr(me_config.slot_options[slot_num], "threshold")!=NULL){
		threshold = get_option_int_val(slot_num, "threshold");
		if (threshold <= 0)
		{
			ESP_LOGD(TAG, "threshold wrong format, set default. Slot:%d", slot_num);
			threshold = 0; // default val
		}
	}

	uint8_t inverse  = 0;
	if (strstr(me_config.slot_options[slot_num], "inverse")!=NULL){
		inverse=1;
	}


	if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/tachometer_0")+3];
		sprintf(t_str, "%s/tachometer_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}


	char str[255];
	TickType_t lastWakeTime = xTaskGetTickCount();
	waitForWorkPermit(slot_num);
	
	while (1)	{	
		uint64_t current_time = esp_timer_get_time();
		
		int current_count;
		ESP_ERROR_CHECK(pcnt_unit_get_count(var.pcnt_unit, &current_count));
		
		// Calculate RPM based on pulse count difference over 1 second
		int pulse_diff = current_count - var.last_count;
		// Add pulse count to buffer (pulses per second)
		pulse_buffer_add(&var.pulse_buffer, (uint16_t)pulse_diff);
		
		// Calculate RPM from pulse buffer
		var.rpm = pulse_buffer_get_rpm(&var.pulse_buffer, var.divider, var.accumulation_time_ms);
		var.last_count = current_count;
		
		// ESP_LOGD(TAG, "Count: %d, Pulses/sec: %d, RPM: %d, Accum time: %lu ms, Buffer size: %d", 
		// 		current_count, pulse_diff, var.rpm, var.accumulation_time_ms, var.pulse_buffer.size);

		
		if (var.prev_rpm != var.rpm){	
			if(threshold==0){
				resault = var.rpm;
				flag_report=1;
			}else{
				if(var.rpm>=threshold){
					resault=!inverse;
				}else{
					resault = inverse;
				}
				if(resault!=prev_resault){
					flag_report=1;
					prev_resault = resault;
				}
			}
			var.prev_rpm=var.rpm;
		}

		if(flag_report){
			flag_report=0;

			memset(str, 0, strlen(str));
			sprintf(str, "%d", resault);

			report(str, slot_num);
			//free(str); 
		}
			
		if (xTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(refresh_period_ms)) == pdFALSE) {
			ESP_LOGE(TAG, "Delay missed! Adjusting wake time.");
			lastWakeTime = xTaskGetTickCount(); // Reset wake time
		}
	}
}

void start_tachometer_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	// int slot_num = *(int*) arg;
	
	xTaskCreate(tachometer_task, "tachometer_task", 1024 * 8, &t_slot_num, 12, NULL);
	// printf("----------getTime:%lld\r\n", esp_timer_get_time());

	ESP_LOGD(TAG, "tachometer_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}