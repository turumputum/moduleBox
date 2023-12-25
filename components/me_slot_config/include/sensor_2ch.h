#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define INDEPENDENT_MODE 0
#define OR_LOGIC_MODE 1
#define AND_LOGIC_MODE 2


void start_sensors_task(int slot_num);