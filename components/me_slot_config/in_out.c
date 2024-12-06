
#include "in_out.h"
#include "sdkconfig.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_timer.h"

#include "executor.h"
#include "esp_log.h"
#include "me_slot_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "IN_OUT";

static void IRAM_ATTR gpio_isr_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

static void gpio_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

void set_out_level(void* arg){
	out_level_cmd_t cmd = *(out_level_cmd_t*)arg;
	gpio_set_level(cmd.gpio_num, cmd.level);
	ESP_LOGD(TAG, "Set level: %ld for slot: %d", cmd.level, cmd.slot_num);
}


void in_task(void *arg){
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][0];

	me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));

	gpio_reset_pin(pin_num);
	esp_rom_gpio_pad_select_gpio(pin_num);
    gpio_config_t in_conf = {};
   	in_conf.intr_type = GPIO_INTR_ANYEDGE;
    //bit mask of the pins, use GPIO4/5 here
    in_conf.pin_bit_mask = (1ULL<<pin_num);
    //set as input mode
    in_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&in_conf);
	gpio_set_intr_type(pin_num, GPIO_INTR_ANYEDGE);
    gpio_install_isr_service(0);
	gpio_isr_handler_add(pin_num, gpio_isr_handler, (void*)slot_num);
	
	// esp_rom_gpio_pad_select_gpio(pin_num);
	// gpio_set_pull_mode(pin_num, GPIO_PULLDOWN_ONLY);
	// gpio_set_direction(pin_num, GPIO_MODE_INPUT);

	ESP_LOGD(TAG,"SETUP IN_pin_%d Slot:%d", pin_num, slot_num );
	char str[255];

	int IN_inverse=0;
	if (strstr(me_config.slot_options[slot_num], "inInverse")!=NULL){
		IN_inverse=1;
	}

	//---set delay---
	uint16_t delay_ms = 0;
	if (strstr(me_config.slot_options[slot_num], "inReportDelay") != NULL) {
		delay_ms = get_option_int_val(slot_num, "inReportDelay");
		ESP_LOGD(TAG, "Set report_delay_ms:%d for slot:%d",delay_ms, slot_num);
	}

	int debounce_gap = 10;
	if (strstr(me_config.slot_options[slot_num], "inDebounceGap") != NULL) {
		debounce_gap = get_option_int_val(slot_num, "inDebounceGap");
		ESP_LOGD(TAG, "Set debounce_gap:%d for slot:%d",debounce_gap, slot_num);
	}
    
    if (strstr(me_config.slot_options[slot_num], "inTopic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "inTopic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/in_0")+3];
		sprintf(t_str, "%s/in_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}


	uint32_t tick=xTaskGetTickCount();
	uint8_t IN_state=0;
	int prev_state=0;
	if(gpio_get_level(pin_num)){
		IN_state=IN_inverse ? 0 : 1;
	}else{
		IN_state=IN_inverse ? 1 : 0;
	}
	memset(str, 0, strlen(str));
	sprintf(str, "%d", IN_state);
	report(str, slot_num);
	prev_state=IN_state;

	esp_timer_handle_t debounce_gap_timer;
	const esp_timer_create_args_t delay_timer_args = {
		.callback = &gpio_handler,
		.arg = (void*)slot_num,
		.name = "debounce_gap_timer"
	};
	esp_timer_create(&delay_timer_args, &debounce_gap_timer);

    for(;;) {
		//vTaskDelay(pdMS_TO_TICKS(10));
		uint8_t tmp;
		if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, portMAX_DELAY) == pdPASS){
			//ESP_LOGD(TAG,"%ld :: Incoming int_msg:%d",xTaskGetTickCount(), tmp);

			if(gpio_get_level(pin_num)){
				IN_state=IN_inverse ? 0 : 1;
			}else{
				IN_state=IN_inverse ? 1 : 0;
			}

			if(debounce_gap!=0){
				if((xTaskGetTickCount()-tick)<debounce_gap){
					//ESP_LOGD(TAG, "Debounce skip delta:%ld",(xTaskGetTickCount()-tick));
					goto exit;
				}
			}
			
			
			if(IN_state != prev_state){
				prev_state=IN_state;

				memset(str, 0, strlen(str));
				sprintf(str, "%d", IN_state);

				if(delay_ms!=0){
					vTaskDelay(pdMS_TO_TICKS(delay_ms));
				}
				report(str, slot_num);
				//ESP_LOGD(TAG,"String:%s", str);
				tick = xTaskGetTickCount();
				if(debounce_gap!=0){
					esp_timer_start_once(debounce_gap_timer, debounce_gap*1000);
				}

			}
			//
			
			exit:
			
		}
    }

}

void start_in_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_in_%d", slot_num);
	xTaskCreatePinnedToCore(in_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"In task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


void out_task(void *arg) {
	int slot_num = *(int*) arg;
	uint32_t heapBefore = xPortGetFreeHeapSize();
	//---init hardware---
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
	//printf("slot:%d pin:%d \r\n", slot_num, pin_num);
	esp_rom_gpio_pad_select_gpio(pin_num);
	gpio_set_direction(pin_num, GPIO_MODE_OUTPUT);


    //---set inverse---
	int out_inverse = 0;
	if (strstr(me_config.slot_options[slot_num], "outInverse") != NULL) {
		out_inverse = 1;
		ESP_LOGD(TAG, "Set out_inverse:%d for slot:%d",out_inverse, slot_num);
	}
	//---set default state---
	uint8_t def_state = out_inverse;
	if (strstr(me_config.slot_options[slot_num], "outDefaultState") != NULL) {
		def_state = get_option_int_val(slot_num, "outDefaultState");
		ESP_LOGD(TAG, "Set def_state:%d for slot:%d",def_state, slot_num);
	}
	gpio_set_level(pin_num, out_inverse ? !def_state : def_state);

	//---set delay---
	uint16_t delay_ms = 0;
	if (strstr(me_config.slot_options[slot_num], "outDelay") != NULL) {
		delay_ms = get_option_int_val(slot_num, "outDelay");
		ESP_LOGD(TAG, "Set delay_ms:%d for slot:%d",delay_ms, slot_num);
	}

	//---set impulse---
	uint16_t impulse_ms = 0;
	if (strstr(me_config.slot_options[slot_num], "outImpulse") != NULL) {
		impulse_ms = get_option_int_val(slot_num, "outImpulse");
		ESP_LOGD(TAG, "Set impulse_ms:%d for slot:%d",impulse_ms, slot_num);
	}

	//---add action to topic list---
	if (strstr(me_config.slot_options[slot_num], "outTopic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "outTopic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/out_0")+3];
		sprintf(t_str, "%s/out_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	}

	me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));
	
	uint32_t tickToToggle=0;
	while(1){
		command_message_t msg;
		if (xQueueReceive(me_state.command_queue[slot_num], &msg, portMAX_DELAY) == pdPASS){
			//ESP_LOGD(TAG, "incoming command:%s", msg.str+strlen(me_state.action_topic_list[slot_num]));
			int val = atoi(msg.str+strlen(me_state.action_topic_list[slot_num])+1);
			//ESP_LOGD(TAG, "incoming command val:%d", val);
			out_level_cmd_t cmd={
				.gpio_num=pin_num,
				.level=out_inverse ? !val : val,
				.slot_num = slot_num
			};
			
			if(delay_ms<=0){
				set_out_level(&cmd);
			}else{
				esp_timer_handle_t delay_timer;
		
				const esp_timer_create_args_t delay_timer_args = {
					.callback = &set_out_level,
					/* argument specified here will be passed to timer callback function */
					//.arg = (void*) cmd,
					.arg = &cmd,
					.name = "delay"
				};
				ESP_ERROR_CHECK(esp_timer_create(&delay_timer_args, &delay_timer));
				ESP_ERROR_CHECK(esp_timer_start_once(delay_timer, delay_ms*1000));
			}

			if(impulse_ms>0){
				esp_timer_handle_t impulse_timer;
				out_level_cmd_t cmd={
					.gpio_num = pin_num,
					.level = def_state,
					.slot_num = slot_num
				};
				const esp_timer_create_args_t impulse_timer_args = {
					.callback = &set_out_level,
					/* argument specified here will be passed to timer callback function */
					//.arg = (void*) cmd,
					.arg = &cmd,
					.name = "impulse"
				};
				ESP_ERROR_CHECK(esp_timer_create(&impulse_timer_args, &impulse_timer));
				ESP_ERROR_CHECK(esp_timer_start_once(impulse_timer, (delay_ms+impulse_ms)*1000));
			}

		}
	}
}

void start_out_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_out_%d", slot_num);
	xTaskCreatePinnedToCore(out_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"Out task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}



void impulse_off_callback(void* arg){
	int slot_num =(int)arg;

	int out_inverse = 0;
    // search way to replace strstr()
	if (strstr(me_config.slot_options[slot_num], "out_inverse") != NULL) {
		out_inverse = 1;
	}
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
	int level = out_inverse ? 1 : 0;
	gpio_set_level(pin_num, level);

	ESP_LOGD(TAG, "Impulse off: %d level: %d", slot_num, level);
}

void exec_out(int slot_num, int payload) {
	int out_inverse = 0;

    // search way to replace strstr()
	if (strstr(me_config.slot_options[slot_num], "out_inverse") != NULL) {
		out_inverse = 1;
	}

	int impulse_len =-1;
	if (strstr(me_config.slot_options[slot_num], "impulse") != NULL) {
		impulse_len = get_option_int_val(slot_num, "impulse");
		
		esp_timer_handle_t oneshot_timer;
		
		const esp_timer_create_args_t oneshot_timer_args = {
            .callback = &impulse_off_callback,
            /* argument specified here will be passed to timer callback function */
            .arg = (void*) slot_num,
            .name = "one-shot"
    	};
		ESP_ERROR_CHECK(esp_timer_create(&oneshot_timer_args, &oneshot_timer));
		ESP_ERROR_CHECK(esp_timer_start_once(oneshot_timer, impulse_len*1000));
	}

	

	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
	int level = out_inverse ? !payload : payload;
	gpio_set_level(pin_num, level);
	//printf("pin_num:%d\n", pin_num);
	ESP_LOGD(TAG, "Led set:%d for slot:%d inverse:%d level:%d", payload, slot_num, out_inverse, level);


}



//-----------------------------out_3ch-------------------------
void out_3ch_task(void *arg) {
	int slot_num = *(int*) arg;
	uint32_t heapBefore = xPortGetFreeHeapSize();
	//---init hardware---
	uint8_t pin_map[3] = {0};
	uint8_t inverse_map[3] = {0};
	pin_map[0] = SLOTS_PIN_MAP[slot_num][0];
	esp_rom_gpio_pad_select_gpio(pin_map[0]);
	gpio_set_direction(pin_map[0], GPIO_MODE_OUTPUT);

	pin_map[1] = SLOTS_PIN_MAP[slot_num][1];
	esp_rom_gpio_pad_select_gpio(pin_map[1]);
	gpio_set_direction(pin_map[1], GPIO_MODE_OUTPUT);

	pin_map[2] = SLOTS_PIN_MAP[slot_num][2];
	esp_rom_gpio_pad_select_gpio(pin_map[2]);
	gpio_set_direction(pin_map[2], GPIO_MODE_OUTPUT);



    //---set inverse---
	if (strstr(me_config.slot_options[slot_num], "inverse_0") != NULL) {
		inverse_map[0] = 1;
		ESP_LOGD(TAG, "Set inverse_0:%d for slot:%d",inverse_map[0], slot_num);
	}
	if (strstr(me_config.slot_options[slot_num], "inverse_1") != NULL) {
		inverse_map[1] = 1;
		ESP_LOGD(TAG, "Set inverse_1:%d for slot:%d",inverse_map[0], slot_num);
	}
	if (strstr(me_config.slot_options[slot_num], "inverse_2") != NULL) {
		inverse_map[2] = 1;
		ESP_LOGD(TAG, "Set inverse_1:%d for slot:%d",inverse_map[0], slot_num);
	}

	//---set delay---
	uint16_t delay_ms = 0;
	if (strstr(me_config.slot_options[slot_num], "outDelay") != NULL) {
		delay_ms = get_option_int_val(slot_num, "outDelay");
		ESP_LOGD(TAG, "Set delay_ms:%d for slot:%d",delay_ms, slot_num);
	}

	//---set impulse---
	uint16_t impulse_ms = 0;
	if (strstr(me_config.slot_options[slot_num], "outImpulse") != NULL) {
		impulse_ms = get_option_int_val(slot_num, "outImpulse");
		ESP_LOGD(TAG, "Set impulse_ms:%d for slot:%d",impulse_ms, slot_num);
	}

	//---add action to topic list---
	if (strstr(me_config.slot_options[slot_num], "outTopic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "outTopic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/out_0")+3];
		sprintf(t_str, "%s/out_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	}

	me_state.command_queue[slot_num] = xQueueCreate(50, sizeof(command_message_t));
	
	
	//---set default state---
	uint8_t state[3]={inverse_map[0],inverse_map[1],inverse_map[2]};
	vTaskDelay(pdTICKS_TO_MS(100));
	if (strstr(me_config.slot_options[slot_num], "defState_0") != NULL) {
		state[0] = get_option_int_val(slot_num, "defState_0");
		if(inverse_map[0]==1)state[0] = !state[0];
	}
	gpio_set_level(pin_map[0], (uint32_t )state[0]);
	if (strstr(me_config.slot_options[slot_num], "defState_1") != NULL) {
		state[1] = get_option_int_val(slot_num, "defState_1");
		if(inverse_map[1]==1)state[1] = !state[1];
	}
	gpio_set_level(pin_map[1], (uint32_t )state[1]);
	if (strstr(me_config.slot_options[slot_num], "defState_2") != NULL) {
		state[2] = get_option_int_val(slot_num, "defState_2");
		if(inverse_map[2]==1)state[2] = !state[2];
	}
	gpio_set_level(pin_map[2], (uint32_t )state[2]);

	while(1){
		command_message_t msg;
		if (xQueueReceive(me_state.command_queue[slot_num], &msg, portMAX_DELAY) == pdPASS){
			//ESP_LOGD(TAG, "incoming command:%s", msg.str+strlen(me_state.action_topic_list[slot_num]));
			char* payload;
			char* cmd;
			int val = 0;
			if(strstr(msg.str, ":")!=NULL){
				cmd = strtok_r(msg.str, ":", &payload);
				val = atoi(payload);
			}else{
				cmd = strdup(msg.str);
			}

			int pin_index=0;
			if(strstr(msg.str, "ch_0")!=NULL){
				pin_index = 0;
			}else if(strstr(msg.str, "ch_1")!=NULL){
				pin_index = 1;
			}else if(strstr(msg.str, "ch_2")!=NULL){
				pin_index = 2;
			}
			
			if(strstr(msg.str, "toggle")!=NULL){
				state[pin_index] = !state[pin_index];
			}else{
				state[pin_index] = inverse_map[pin_index] ? !val : val;
			}

			//ESP_LOGD(TAG, "incoming command val:%d", val);
			out_level_cmd_t port_cmd={
				.gpio_num=pin_map[pin_index],
				.level=state[pin_index],
				.slot_num = slot_num
			};
			
			if(delay_ms<=0){
				set_out_level(&port_cmd);
			}else{
				esp_timer_handle_t delay_timer;
		
				const esp_timer_create_args_t delay_timer_args = {
					.callback = &set_out_level,
					/* argument specified here will be passed to timer callback function */
					//.arg = (void*) cmd,
					.arg = &port_cmd,
					.name = "delay"
				};
				ESP_ERROR_CHECK(esp_timer_create(&delay_timer_args, &delay_timer));
				ESP_ERROR_CHECK(esp_timer_start_once(delay_timer, delay_ms*1000));
			}

			if(impulse_ms>0){
				esp_timer_handle_t impulse_timer;
				state[pin_index] = !state[pin_index];
				out_level_cmd_t imp_cmd={
					.gpio_num=pin_map[pin_index],
					.level=state[pin_index],
					.slot_num = slot_num	
				};
				const esp_timer_create_args_t impulse_timer_args = {
					.callback = &set_out_level,
					/* argument specified here will be passed to timer callback function */
					//.arg = (void*) cmd,
					.arg = &imp_cmd,
					.name = "impulse"
				};
				ESP_ERROR_CHECK(esp_timer_create(&impulse_timer_args, &impulse_timer));
				ESP_ERROR_CHECK(esp_timer_start_once(impulse_timer, (delay_ms+impulse_ms)*1000));
			}

		}
	}
}

void start_out_3ch_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_out_3ch_%d", slot_num);
	xTaskCreatePinnedToCore(out_3ch_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"Out task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


//-----------------------------in_3ch-------------------------
void readPins(uint8_t *pin_mass, uint8_t *inverse, int numOfCh, uint8_t *state_mass){
	for(int i=0; i<numOfCh; i++){
		state_mass[i] = gpio_get_level(pin_mass[i])? !inverse[i] : inverse[i];
	}
	//ESP_LOGD(TAG, "readPins: %d %d %d  inverseMap:%d %d %d", state_mass[0], state_mass[1], state_mass[2], inverse[0], inverse[1], inverse[2]);
}

void checkStates(uint8_t *state_mass, uint8_t *prev_state_mass, int numOfCh, uint8_t slot_num, uint8_t logic){
	//ESP_LOGD(TAG, "readPins: %d %d %d  prev_state:%d %d %d", state_mass[0], state_mass[1], state_mass[2], prev_state_mass[0], prev_state_mass[1], prev_state_mass[2]);
	for(int i=0; i<numOfCh; i++){
		if(state_mass[i]!=prev_state_mass[i]){
			char t_str[255];
			memset(t_str, 0, sizeof(t_str));
			if(logic==INDEPENDENT_MODE){
				sprintf(t_str, "/ch_%d:%d", i, state_mass[i]);
			}else if(logic==OR_LOGIC_MODE){
				int summ=0;
				for(int j=0; j<numOfCh; j++){
					if(state_mass[j])summ++;
				}
				if(summ==0){
					sprintf(t_str, "%d", 0);
				}else{
					int pr_summ=0;
					for(int j=0; j<numOfCh; j++){
						if(prev_state_mass[j])pr_summ++;
					}
					if(pr_summ==0){
						sprintf(t_str, "%d", 1);
					}else{
						goto skip;
					}
				}
			}else if(logic==AND_LOGIC_MODE){
				int summ=0;
				for(int j=0; j<numOfCh; j++){
					if(state_mass[j])summ++;
				}
				if(summ==numOfCh){
					sprintf(t_str, "%d", 1);
				}else{
					int pr_summ=0;
					for(int j=0; j<numOfCh; j++){
						if(prev_state_mass[j])pr_summ++;
					}
					if(pr_summ==numOfCh){
						sprintf(t_str, "%d", 0);
					}else{
						goto skip;
					}
				}
			}
			report(t_str, slot_num);
			//ESP_LOGD(TAG, "report: %s", t_str);
			skip:
			prev_state_mass[i] = state_mass[i];
		}
	}
}


void in_3ch_task(void *arg){
	int slot_num = *(int*) arg;
	uint8_t numOfCh=3;
	uint8_t pin_mass[numOfCh];
	for(int i=0; i<numOfCh; i++){
		pin_mass[i] = SLOTS_PIN_MAP[slot_num][i];
		gpio_reset_pin(pin_mass[i]);
		esp_rom_gpio_pad_select_gpio(pin_mass[i]);
		gpio_config_t in_conf = {};
		in_conf.intr_type = GPIO_INTR_ANYEDGE;
		//bit mask of the pins, use GPIO4/5 here
		in_conf.pin_bit_mask = (1ULL<<pin_mass[i]);
		//set as input mode
		in_conf.mode = GPIO_MODE_INPUT;
		gpio_config(&in_conf);
		gpio_set_intr_type(pin_mass[i], GPIO_INTR_ANYEDGE);
		gpio_install_isr_service(0);
		gpio_isr_handler_add(pin_mass[i], gpio_isr_handler, (void*)slot_num);
		ESP_LOGD(TAG,"SETUP IN_pin_%d Slot:%d", pin_mass[i], slot_num );
	}

	me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));

	
	

	uint8_t inverse[3]={0,0,0};
	if (strstr(me_config.slot_options[slot_num], "inverse_0")!=NULL){
		inverse[0]=1;
	}
	if (strstr(me_config.slot_options[slot_num], "inverse_1")!=NULL){
		inverse[1]=1;
	}
	if (strstr(me_config.slot_options[slot_num], "inverse_2")!=NULL){
		inverse[2]=1;
	}
	//ESP_LOGD(TAG, "inverse[0]:%d, inverse[1]:%d, inverse[2]:%d", inverse[0], inverse[1], inverse[2]);

	//---set delay---
	uint16_t delay_ms = 0;
	if (strstr(me_config.slot_options[slot_num], "inReportDelay") != NULL) {
		delay_ms = get_option_int_val(slot_num, "inReportDelay");
		ESP_LOGD(TAG, "Set report_delay_ms:%d for slot:%d",delay_ms, slot_num);
	}

	int debounce_gap = 30;
	if (strstr(me_config.slot_options[slot_num], "inDebounceGap") != NULL) {
		debounce_gap = get_option_int_val(slot_num, "inDebounceGap");
		ESP_LOGD(TAG, "Set debounce_gap:%d for slot:%d",debounce_gap, slot_num);
	}

	int mode = INDEPENDENT_MODE;
	if (strstr(me_config.slot_options[slot_num], "logic") != NULL) {
        char* mode_str=NULL;
		mode_str = get_option_string_val(slot_num, "logic");
        if(strstr(mode_str,"or")!=NULL){
            mode= OR_LOGIC_MODE;
        }else if(strstr(mode_str,"and")!=NULL){
            mode= AND_LOGIC_MODE;
        }
		ESP_LOGD(TAG, "Set logic:%d for slot:%d",mode, slot_num);
	}
    
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/in_0")+3];
		sprintf(t_str, "%s/in_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}


	uint32_t tick=xTaskGetTickCount();
	uint8_t state[3]={0,0,0};
	uint8_t prev_state[3]={255,255,255};
	//ESP_LOGD(TAG, "inverse[0]:%d, inverse[1]:%d, inverse[2]:%d", inverse[0], inverse[1], inverse[2]);
	
	vTaskDelay(pdMS_TO_TICKS(100));
	readPins(&pin_mass, &inverse, numOfCh, &state);
	checkStates(&state, &prev_state, numOfCh, slot_num, mode);

	esp_timer_handle_t debounce_gap_timer;
	const esp_timer_create_args_t delay_timer_args = {
		.callback = &gpio_handler,
		.arg = (void*)slot_num,
		.name = "debounce_gap_timer"
	};
	esp_timer_create(&delay_timer_args, &debounce_gap_timer);

    for(;;) {
		//vTaskDelay(pdMS_TO_TICKS(10));
		uint8_t tmp;
		if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, portMAX_DELAY) == pdPASS){
			//ESP_LOGD(TAG,"%ld :: Incoming int_msg:%d",xTaskGetTickCount(), tmp);

			readPins(&pin_mass, &inverse, numOfCh, &state);
	
			if(debounce_gap!=0){
				if((xTaskGetTickCount()-tick)<debounce_gap){
					//ESP_LOGD(TAG, "Debounce skip delta:%ld",(xTaskGetTickCount()-tick));
					goto exit;
				}
			}
			
			checkStates(&state, &prev_state, numOfCh, slot_num, mode);
			//
			
			exit:
			
		}
    }

}

void start_in_3ch_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_in_3ch_%d", slot_num);
	xTaskCreatePinnedToCore(in_3ch_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"In_3 task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


void in_2ch_task(void *arg){
	int slot_num = *(int*) arg;
	uint8_t numOfCh=2;
	uint8_t pin_mass[numOfCh];
	for(int i=0; i<numOfCh; i++){
		pin_mass[i] = SLOTS_PIN_MAP[slot_num][i];
		gpio_reset_pin(pin_mass[i]);
		esp_rom_gpio_pad_select_gpio(pin_mass[i]);
		gpio_config_t in_conf = {};
		in_conf.intr_type = GPIO_INTR_ANYEDGE;
		//bit mask of the pins, use GPIO4/5 here
		in_conf.pin_bit_mask = (1ULL<<pin_mass[i]);
		//set as input mode
		in_conf.mode = GPIO_MODE_INPUT;
		gpio_config(&in_conf);
		gpio_set_intr_type(pin_mass[i], GPIO_INTR_ANYEDGE);
		gpio_install_isr_service(0);
		gpio_isr_handler_add(pin_mass[i], gpio_isr_handler, (void*)slot_num);
		ESP_LOGD(TAG,"SETUP IN_pin_%d Slot:%d", pin_mass[i], slot_num );
	}

	me_state.interrupt_queue[slot_num] = xQueueCreate(15, sizeof(uint8_t));

	
	

	uint8_t inverse[3]={0,0,0};
	if (strstr(me_config.slot_options[slot_num], "inverse_0")!=NULL){
		inverse[0]=1;
	}
	if (strstr(me_config.slot_options[slot_num], "inverse_1")!=NULL){
		inverse[1]=1;
	}

	//ESP_LOGD(TAG, "inverse[0]:%d, inverse[1]:%d, inverse[2]:%d", inverse[0], inverse[1], inverse[2]);

	//---set delay---
	uint16_t delay_ms = 0;
	if (strstr(me_config.slot_options[slot_num], "inReportDelay") != NULL) {
		delay_ms = get_option_int_val(slot_num, "inReportDelay");
		ESP_LOGD(TAG, "Set report_delay_ms:%d for slot:%d",delay_ms, slot_num);
	}

	int debounce_gap = 30;
	if (strstr(me_config.slot_options[slot_num], "inDebounceGap") != NULL) {
		debounce_gap = get_option_int_val(slot_num, "inDebounceGap");
		ESP_LOGD(TAG, "Set debounce_gap:%d for slot:%d",debounce_gap, slot_num);
	}

	int mode = INDEPENDENT_MODE;
	if (strstr(me_config.slot_options[slot_num], "logic") != NULL) {
        char* mode_str=NULL;
		mode_str = get_option_string_val(slot_num, "logic");
        if(strstr(mode_str,"or")!=NULL){
            mode= OR_LOGIC_MODE;
        }else if(strstr(mode_str,"and")!=NULL){
            mode= AND_LOGIC_MODE;
        }
		ESP_LOGD(TAG, "Set logic:%d for slot:%d",mode, slot_num);
	}
    
    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/in_0")+3];
		sprintf(t_str, "%s/in_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}


	uint32_t tick=xTaskGetTickCount();
	uint8_t state[3]={0,0,0};
	uint8_t prev_state[3]={255,255,255};
	//ESP_LOGD(TAG, "inverse[0]:%d, inverse[1]:%d, inverse[2]:%d", inverse[0], inverse[1], inverse[2]);
	
	vTaskDelay(pdMS_TO_TICKS(100));
	readPins(&pin_mass, &inverse, numOfCh, &state);
	checkStates(&state, &prev_state, numOfCh, slot_num, mode);

	esp_timer_handle_t debounce_gap_timer;
	const esp_timer_create_args_t delay_timer_args = {
		.callback = &gpio_handler,
		.arg = (void*)slot_num,
		.name = "debounce_gap_timer"
	};
	esp_timer_create(&delay_timer_args, &debounce_gap_timer);

    for(;;) {
		//vTaskDelay(pdMS_TO_TICKS(10));
		uint8_t tmp;
		if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, portMAX_DELAY) == pdPASS){
			//ESP_LOGD(TAG,"%ld :: Incoming int_msg:%d",xTaskGetTickCount(), tmp);

			readPins(&pin_mass, &inverse, numOfCh, &state);
	
			if(debounce_gap!=0){
				if((xTaskGetTickCount()-tick)<debounce_gap){
					//ESP_LOGD(TAG, "Debounce skip delta:%ld",(xTaskGetTickCount()-tick));
					goto exit;
				}
			}
			
			checkStates(&state, &prev_state, numOfCh, slot_num, mode);
			//
			
			exit:
			
		}
    }

}

void start_in_2ch_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_in_2ch_%d", slot_num);
	xTaskCreatePinnedToCore(in_2ch_task, tmpString, 1024*4, &t_slot_num,12, NULL, 1);

	ESP_LOGD(TAG,"In_2 task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}