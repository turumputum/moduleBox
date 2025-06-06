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
#define INDEPENDENT_MODE 0
#define OR_LOGIC_MODE 1
#define AND_LOGIC_MODE 2

typedef struct {
    uint8_t pinMass[3];
	uint8_t inverseMass[3];
	int8_t statMass[3];
	int8_t prevStateMass[3];
	uint8_t numOfCh;
	uint8_t slot_num;
	uint8_t logic; 
} inData_t;

#define inData_DEFAULT() {\
	.pinMass = {0},\
	.inverseMass = {0},\
	.statMass = {0},\
	.prevStateMass = {-1},\
	.numOfCh = 0,\
	.slot_num = 0,\
	.logic = 0,\
}

typedef struct{
	uint8_t slot_num;
	gpio_num_t gpio_num;
	uint32_t level;
} out_level_cmd_t;


//void init_out(int slot_num);
void start_in_task(int slot_num);
void start_in_2ch_task(int slot_num);
void start_in_3ch_task(int slot_num, int numOfCh);
void start_out_task(int slot_num);
void start_out_2ch_task(int slot_num);
void start_out_3ch_task(int slot_num);
void exec_out(int slot_num, int payload);