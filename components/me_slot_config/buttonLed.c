// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "buttonLed.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#include "driver/ledc.h"
#include "esp_timer.h"
#include "reporter.h"
#include "executor.h"
#include "stateConfig.h"

#include "esp_log.h"
#include "me_slot_config.h"

#include <generated_files/buttonLed.h>

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_BUTTONLEDCONFIG
{
	int 					button_inverse;
	int 					debounce_gap;
	uint8_t 				led_inverse;
	int16_t 				fade_increment;
	int16_t 				maxBright;
	int16_t 				minBright;
	uint16_t 				refreshPeriod;
	int 					animate;

} BUTTONLEDCONFIG, * PBUTTONLEDCONFIG; 


// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

extern const uint8_t gamma_8[256];

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "BUTTONS";

enum animation{
	NONE,
	FLASH,
	GLITCH
};

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

static void IRAM_ATTR timer_isr_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
	me_state.counters[slot_num].flag = 1;
    xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

static void IRAM_ATTR gpio_isr_handler(void* arg){
    int slot_num = (int) arg;
	uint8_t tmp=1;
	xQueueSendFromISR(me_state.interrupt_queue[slot_num], &tmp, NULL);
}

/* 
    Модуль светодиодной кнопки
*/
void configure_button_led(PBUTTONLEDCONFIG ch, int slot_num, int mode)
{
	if (mode == 0) // Button
	{
		if (strstr(me_config.slot_options[slot_num], "buttonInverse")!=NULL){
			/* Флаг определяет инверсию кнопки
			*/
			ch->button_inverse = get_option_flag_val(slot_num, "buttonInverse");
		}

		if (strstr(me_config.slot_options[slot_num], "buttonDebounceGap") != NULL) {
			/* Глубина фильтра дребезга
			*/
			ch->debounce_gap = get_option_int_val(slot_num, "buttonDebounceGap", "", 10, 1, 4096);
			ESP_LOGD(TAG, "Set debounce_gap:%d for slot:%d",ch->debounce_gap, slot_num);
		}

		if (strstr(me_config.slot_options[slot_num], "buttonTopic") != NULL) {
			/* Топик для событий кнопки
			*/
			char * custom_topic = get_option_string_val(slot_num, "buttonTopic");
			me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
			ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
		}else{
			char t_str[strlen(me_config.deviceName)+strlen("/button_0")+3];
			sprintf(t_str, "%s/button_%d",me_config.deviceName, slot_num);
			me_state.trigger_topic_list[slot_num]=strdup(t_str);
			ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
		}
	}
	else
	{
		if (strstr(me_config.slot_options[slot_num], "ledInverse")!=NULL){
			/* Флаг определяет инверсию кнопки
			*/
			ch->led_inverse = get_option_flag_val(slot_num, "ledInverse");
		}

		ch->fade_increment = 255;
		if (strstr(me_config.slot_options[slot_num], "increment") != NULL) {
			/* Интенсивность затухание свечения
			*/
			ch->fade_increment = get_option_int_val(slot_num, "increment", "", 10, 1, 4096);
			ESP_LOGD(TAG, "Set fade_increment:%d for slot:%d",ch->fade_increment, slot_num);
		}

		ch->maxBright = 255;
		if (strstr(me_config.slot_options[slot_num], "maxBright") != NULL) {
			/* Максимальное свечение
			*/
			ch->maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 4095);
			if(ch->maxBright>255)ch->maxBright=255;
			if(ch->maxBright<0)ch->maxBright=0;
			ESP_LOGD(TAG, "Set maxBright:%d for slot:%d", ch->maxBright, slot_num);
		}

		ch->minBright = 0;
		if (strstr(me_config.slot_options[slot_num], "minBright") != NULL) {
			/* Минимальное свечение
			*/
			ch->minBright = get_option_int_val(slot_num, "minBright", "", 10, 1, 4096);
			if(ch->minBright>255)ch->minBright=255;
			if(ch->minBright<0)ch->minBright=0;
			ESP_LOGD(TAG, "Set minBright:%d for slot:%d", ch->minBright, slot_num);
		}

		ch->refreshPeriod = 40;
		if (strstr(me_config.slot_options[slot_num], "refreshRate") != NULL) {
			/* Период обновления
			*/
			ch->refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 10, 1, 4096));
			ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d",ch->refreshPeriod, slot_num);
		}

		ch->animate = NONE;

		if (strstr(me_config.slot_options[slot_num], "ledMode") != NULL) {
			/* Задаёт режим анимации */
			if ((ch->animate = get_option_enum_val(slot_num, "ledMode", "none", "flash", "glitch", NULL)) < 0)
			{
				ESP_LOGE(TAG, "animate: unricognized value");
				ch->animate = NONE;
			}
			else
				ESP_LOGD(TAG, "Custom animate: %d", ch->animate);
		}

		if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
			char* custom_topic=NULL;
			/* Топик для режима свечения
			*/
			custom_topic = get_option_string_val(slot_num, "ledTopic");
			me_state.action_topic_list[slot_num]=strdup(custom_topic);
			ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
		}else{
			char t_str[strlen(me_config.deviceName)+strlen("/led_0")+3];
			sprintf(t_str, "%s/led_%d",me_config.deviceName, slot_num);
			me_state.action_topic_list[slot_num]=strdup(t_str);
			ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
		}
	}
}

void button_task(void *arg)
{
	BUTTONLEDCONFIG		c = {0};

	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][0];

	me_state.interrupt_queue[slot_num] = xQueueCreate(10, sizeof(uint8_t));

	gpio_reset_pin(pin_num);
	esp_rom_gpio_pad_select_gpio(pin_num);
    gpio_config_t in_conf = {};
   	in_conf.intr_type = GPIO_INTR_ANYEDGE;
    in_conf.pin_bit_mask = (1ULL<<pin_num);
	in_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    in_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&in_conf);
	gpio_set_intr_type(pin_num, GPIO_INTR_ANYEDGE);
    gpio_install_isr_service(0);
	gpio_isr_handler_add(pin_num, gpio_isr_handler, (void*)slot_num);
	
	ESP_LOGD(TAG,"SETUP BUTTON_pin_%d Slot:%d", pin_num, slot_num );
	
	char str[255];

	configure_button_led(&c, slot_num, 0);

	int8_t button_state=0;
	int prev_state=-1;
	if(gpio_get_level(pin_num)){
		button_state=c.button_inverse ? 0 : 1;
	}else{
		button_state=c.button_inverse ? 1 : 0;
	}

	me_state.counters[slot_num].pin_num = pin_num;

	// memset(str, 0, strlen(str));
	// sprintf(str, "%d", button_state);
	// report(str, slot_num);

	esp_timer_handle_t debounce_gap_timer;
	const esp_timer_create_args_t delay_timer_args = {
		.callback = &timer_isr_handler,
		.arg = (void*)slot_num,
		.name = "debounce_gap_timer"
	};

	if (c.debounce_gap)
	{
		esp_timer_create(&delay_timer_args, &debounce_gap_timer);
		esp_timer_start_periodic(debounce_gap_timer, c.debounce_gap*1000);
	}
	
	waitForWorkPermit(slot_num);
    //while (workIsPermitted(slot_num))
	while (true)
	{
		if (button_state != prev_state) // Если состояние кнопки таки изменилось
		{
			// фиксируем состояние
			prev_state = button_state;

			// отчёт
			memset(str, 0, strlen(str));
			sprintf(str, "%d", button_state);
			report(str, slot_num);
			
			//vTaskDelay(pdMS_TO_TICKS(5));
			ESP_LOGD(TAG,"BUTTON report String:%s", str);
		}

		// Получаем сигнал от GPIO или таймера
		uint8_t tmp;
		if (xQueueReceive(me_state.interrupt_queue[slot_num], &tmp, portMAX_DELAY) == pdPASS)
		{
			debounceStat_t * st = &me_state.counters[slot_num];

			if (gpio_get_level(pin_num))
			{
				st->ones++;
			}

			st->total++;

			int newState = -1;

			if (c.debounce_gap)	// Если таймер активен
			{
				if (st->flag) 	// Если сигнал был от таймера
				{
					//ESP_LOGD(TAG,"%ld :: Incoming int_msg:%d",xTaskGetTickCount(), tmp);

					// Фильтрация дребезга заключается в следующем: 
					// читаем состояния за фиксированный промежуток времени и если единиц было считаено >= 50%,
					// то состояние считается "1" (кнопку определённо давили), иначе "0" 

					if (st->total > 0) // Если вообще есть отчёты
					{
						if (st->ones > 0) // Если вообще есть единицы
						{
							// Если единиц больше или равно половине
							if (st->ones >= (st->total >> 1))
							{
								newState = 1;
							}
						}
						else	
							newState = 0;
					}

					st->flag = 0;
				}
			}
			else	// Таймер не активен
			{
				

				newState = st->ones;
			}

			if (newState >= 0)	// Если было изменение состояния кнопки
			{
				button_state = (c.button_inverse ? ~newState : newState) & 1;

				st->ones 	= 0;
				st->total 	= 0;
			}
		}
    }

}

void start_button_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "task_button_%d", slot_num);
	xTaskCreatePinnedToCore(button_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES-5, NULL, 0);

	ESP_LOGD(TAG,"Button task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


void checkBright(uint8_t *currentBright, uint8_t targetBright, uint8_t fade_increment){
	if(*currentBright!=targetBright){
        if(*currentBright < targetBright){
            if((targetBright - *currentBright) < fade_increment){
              	*currentBright = targetBright;
            }else{
                *currentBright += fade_increment;
            }
        }else if(*currentBright > targetBright){
            if((*currentBright - targetBright) < fade_increment){
               	*currentBright = targetBright;
            }else{
                *currentBright -= fade_increment;
            }
        }
    }
}

void led_task(void *arg){
	BUTTONLEDCONFIG c;
    uint32_t startTick = xTaskGetTickCount();
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
    uint8_t state = 0;

	me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    //vQueueAddToRegistry( xQueue, "AMeaningfulName" );

	configure_button_led(&c, slot_num, 1);

	// Prepare and then apply the LEDC PWM timer configuration
	//todo ledc timer config!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .timer_num        = LEDC_TIMER_0,
        .freq_hz          = 4000,  // Set output frequency at 4 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    if((LEDC_CHANNEL_MAX - me_state.ledc_chennelCounter)<1){
		ESP_LOGE(TAG, "LEDC channel has ended");
		goto EXIT;
	}
	
	ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = me_state.ledc_chennelCounter++,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = pin_num,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    ESP_LOGD(TAG, "Led task config end. Slot_num:%d, duration_ms:%ld", slot_num, pdTICKS_TO_MS(xTaskGetTickCount()-startTick));


    int16_t currentBright=0;
	int16_t appliedBright = -1;
    int16_t targetBright=c.led_inverse ? c.maxBright : c.minBright;

	

	TickType_t lastWakeTime = xTaskGetTickCount(); 

	waitForWorkPermit(slot_num);
	
    while (1) {

        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, c.refreshPeriod) == pdPASS){
            ESP_LOGD(TAG, "LED Input command %s for slot:%d", msg.str, slot_num);
			int val = atoi(msg.str+strlen(me_state.action_topic_list[slot_num])+1);
			if(val!=c.led_inverse){
				targetBright = c.maxBright;
			}else{
				targetBright = c.minBright;
			}
        }

        if (c.animate == FLASH){
            if(currentBright<=c.minBright){
				targetBright=c.maxBright;
				//ESP_LOGD(TAG, "Flash min bright:%d targetBright:%d", currentBright, targetBright); 
			}else if(currentBright>=c.maxBright){
				targetBright=c.minBright;
				//ESP_LOGD(TAG, "Flash max bright:%d targetBright:%d", currentBright, targetBright); 
			}
        }

		checkBright(&currentBright, targetBright, c.fade_increment);

		if (currentBright != appliedBright)
		{
			ledc_set_duty(LEDC_LOW_SPEED_MODE, ledc_channel.channel, currentBright);
			ledc_update_duty(LEDC_LOW_SPEED_MODE, ledc_channel.channel);

			appliedBright = currentBright;
		}

		// Слишком жирный делай, перенесено в QueueReceive
        //ESP_LOGD(TAG, "Led delay :%d", delay); 
        //vTaskDelayUntil(&lastWakeTime, refreshPeriod);
    }

	EXIT:
    vTaskDelete(NULL);
}


void start_led_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(led_task, "led_task", 1024*4, &slot_num,12, NULL);
	ESP_LOGD(TAG,"led_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
const char * get_manifest_buttonLed()
{
	return manifesto;
}
