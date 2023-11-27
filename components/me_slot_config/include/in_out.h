#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// typedef struct{
// 	uint32_t gpio_num;
// 	QueueHandle_t gpio_evt_queue;
// } gpio_and_queue_t;

typedef struct{
	uint8_t slot_num;
	gpio_num_t gpio_num;
	uint32_t level;
} out_level_cmd_t;


void init_out(int slot_num);
void start_in_task(int slot_num);
void start_out_task(int slot_num);
void exec_out(int slot_num, int payload);