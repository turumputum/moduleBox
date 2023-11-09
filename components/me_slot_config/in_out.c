
#include "in_out.h"
#include "sdkconfig.h"

#include "reporter.h"
#include "stateConfig.h"
#include "esp_timer.h"

#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[6][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "IN_OUT";

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    gpio_and_queue_t *gpio_and_queue = (gpio_and_queue_t*) arg;
	int gpio_level = gpio_get_level(gpio_and_queue->gpio_num);
    xQueueSendFromISR(gpio_and_queue->gpio_evt_queue, &gpio_level, NULL);
}

void in_task(void *arg){
	gpio_and_queue_t gpio_and_queue;
    gpio_and_queue.gpio_evt_queue = xQueueCreate(10, sizeof(int));

	int num_of_slot = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[num_of_slot][0];
	gpio_reset_pin(pin_num);
	//esp_rom_gpio_pad_select_gpio(pin_num);
    // gpio_config_t in_conf = {};
   	// in_conf.intr_type = GPIO_INTR_ANYEDGE;
    // //bit mask of the pins, use GPIO4/5 here
    // in_conf.pin_bit_mask = (1ULL<<pin_num);
    // //set as input mode
    // in_conf.mode = GPIO_MODE_INPUT;
    // gpio_config(&in_conf);
	// gpio_set_intr_type(pin_num, GPIO_INTR_ANYEDGE);
    // gpio_install_isr_service(0);
	// gpio_isr_handler_add(pin_num, gpio_isr_handler, &gpio_and_queue);
	esp_rom_gpio_pad_select_gpio(pin_num);
	gpio_set_pull_mode(pin_num, GPIO_PULLDOWN_ONLY);
	gpio_set_direction(pin_num, GPIO_MODE_INPUT);

	ESP_LOGD(TAG,"SETUP IN_pin_%d Slot:%d", pin_num, num_of_slot );
	
	int IN_state=0;
	int prev_state=0;
	char str[255];

	int IN_inverse=0;
	if (strstr(me_config.slot_options[num_of_slot], "in_inverse")!=NULL){
		IN_inverse=1;
	}

	uint8_t flag_custom_topic = 0;
    char* custom_topic=NULL;
    if (strstr(me_config.slot_options[num_of_slot], "custom_topic") != NULL) {
    	custom_topic = get_option_string_val(num_of_slot, "custom_topic");
		ESP_LOGD(TAG, "Custom topic:%s", custom_topic);
        flag_custom_topic = 1;
    }

    for(;;) {
		vTaskDelay(pdMS_TO_TICKS(10));
		if(gpio_get_level(pin_num)){
			IN_state=IN_inverse ? 0 : 1;
		}else{
			IN_state=IN_inverse ? 1 : 0;
		}
		if(IN_state != prev_state){
			prev_state = IN_state;

			memset(str, 0, strlen(str));
			//ESP_LOGD(TAG,"button_%d:%d inverse:%d", num_of_slot, button_state, button_inverse );
			if (flag_custom_topic) {
                sprintf(str, "%s:%d", custom_topic, IN_state);
				//ESP_LOGD(TAG,"custom_topic:%s String:%s",custom_topic, str);
            }else {
                sprintf(str, "%s/in_%d:%d", me_config.device_name, num_of_slot, IN_state);
            }
            report(str);
			ESP_LOGD(TAG,"String:%s", str);

		}

        // if(xQueueReceive(gpio_and_queue.gpio_evt_queue, &level, portMAX_DELAY)) {
        //     printf("Slot[%d] intr, val: %d\n", num_of_slot, level);
		// 	memset(str, 0, strlen(str));
		// 	//ESP_LOGD(TAG,"button_%d:%d inverse:%d", num_of_slot, button_state, button_inverse );
		// 	if (flag_custom_topic) {
        //         sprintf(str, "%s:%d", custom_topic, level);
		// 		//ESP_LOGD(TAG,"custom_topic:%s String:%s",custom_topic, str);
        //     }else {
        //         sprintf(str, "%s/in_%d:%d", me_config.device_name, num_of_slot, level);
        //     }
        //     report(str);
		// 	ESP_LOGD(TAG,"String:%s", str);
        // }
    }

}

void start_in_task(int num_of_slot){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = num_of_slot;
	char tmpString[60];
	sprintf(tmpString, "task_in_%d", num_of_slot);
	xTaskCreate(in_task, tmpString, 1024*4, &t_slot_num,12, NULL);

	ESP_LOGD(TAG,"In task created for slot: %d Heap usage: %lu free heap:%u", num_of_slot, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

void init_out(int slot_num) {
	uint32_t heapBefore = xPortGetFreeHeapSize();
	//---init hardware---
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
	//printf("slot:%d pin:%d \r\n", slot_num, pin_num);
	esp_rom_gpio_pad_select_gpio(pin_num);
	gpio_set_direction(pin_num, GPIO_MODE_OUTPUT);


    //---set default state---
	int out_inverse = 0;
	if (strstr(me_config.slot_options[slot_num], "out_inverse") != NULL) {
		out_inverse = 1;
	}
	//---set default state---
	uint8_t def_state = out_inverse;
	if (strstr(me_config.slot_options[slot_num], "out_default_high") != NULL) {
            def_state = !def_state;
	}
	ESP_ERROR_CHECK(gpio_set_level(pin_num, (uint32_t )def_state));

	//---add action to topic list---
	char *str = calloc(strlen(me_config.device_name) + 16, sizeof(char));
	sprintf(str, "%s/out_%d", me_config.device_name, slot_num);
	me_state.action_topic_list[me_state.action_topic_list_index] = str;
	me_state.action_topic_list_index++;

	ESP_LOGD(TAG, "Out inited for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
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