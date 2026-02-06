// ***************************************************************************
// TITLE: Input/Output Module Header
//
// PROJECT: moduleBox
// ***************************************************************************


#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "stdcommand.h"


// =============================================================================
// LOGIC MODES FOR MULTI-CHANNEL INPUTS
// =============================================================================

#define INDEPENDENT_MODE    0
#define OR_LOGIC_MODE       1
#define AND_LOGIC_MODE      2


// =============================================================================
// FUNCTION DECLARATIONS
// =============================================================================

// --- Task start functions ---
void start_in_out_task(int slot_num);
void start_in_2ch_task(int slot_num);
void start_in_3ch_task(int slot_num);
void start_out_2ch_task(int slot_num);



