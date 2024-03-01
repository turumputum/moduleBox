/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Ha Thach for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"

#include "driver/periph_ctrl.h"
#include "driver/rmt.h"

#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "board_api.h"


//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

static esp_timer_handle_t timer_hdl;

extern int main(void);
static void internal_timer_cb(void* arg);

//--------------------------------------------------------------------+
// TinyUSB thread
//--------------------------------------------------------------------+

#ifdef BOOTLDSD_SELF_UPDATE

void app_main(void)
{
  main();
}

#else

void app_main(void)
{
  main();
}

#endif

//--------------------------------------------------------------------+
// Board API
//--------------------------------------------------------------------+

void board_init(void)
{
  // Set up timer
  const esp_timer_create_args_t periodic_timer_args = { .callback = internal_timer_cb };
  esp_timer_create(&periodic_timer_args, &timer_hdl);
}

void board_reset(void)
{
  esp_restart();
}

bool board_app_valid(void)
{
  // esp32s2 is always enter DFU mode
  return false;
}

void board_app_jump(void)
{
  // nothing to do
}

//--------------------------------------------------------------------+
// Timer
//--------------------------------------------------------------------+

static void internal_timer_cb(void*  arg)
{
  (void) arg;
  board_timer_handler();
}

void board_timer_start(uint32_t ms)
{
  esp_timer_stop(timer_hdl);
  esp_timer_start_periodic(timer_hdl, ms*1000);
}

void board_timer_stop(void)
{
  esp_timer_stop(timer_hdl);
}
