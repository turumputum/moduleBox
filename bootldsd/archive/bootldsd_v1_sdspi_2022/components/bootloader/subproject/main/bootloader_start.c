// Copyright 2015-2019 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <stdbool.h>
#include "esp_log.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "bootloader_hooks.h"

// components/esp_rom
#include "esp_rom_sys.h"
#include "esp_rom_gpio.h"

#include "soc/cpu.h"
#include "hal/gpio_ll.h"

// Specific board header specified with -DBOARD=
#include "board.h"

#ifdef TCA9554_ADDR
  #include "hal/i2c_types.h"

  // Using GPIO expander requires long reset delay (somehow)
  #define NEOPIXEL_RESET_DELAY      ns2cycle(1000*1000)
#endif

#ifndef NEOPIXEL_RESET_DELAY
  // Need at least 200 us for initial delay although Neopixel reset time is only 50 us
  #define NEOPIXEL_RESET_DELAY      ns2cycle(200*1000)
#endif


// Reset Reason Hint to enter UF2. Check out esp_reset_reason_t for other Espressif pre-defined values
#define APP_REQUEST_UF2_RESET_HINT   0x11F2

// Initial delay in milliseconds to detect user interaction to enter UF2.
#define UF2_DETECTION_DELAY_MS       500

uint8_t const RGB_DOUBLE_TAP[] = { 0x80, 0x00, 0xff }; // Purple
uint8_t const RGB_OFF[]        = { 0x00, 0x00, 0x00 };

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+
static const char *TAG = "boot";

static int select_partition_number(bootloader_state_t *bs);
static int selected_boot_partition(const bootloader_state_t *bs);

static void board_led_on(void);
static void board_led_off(void);

//--------------------------------------------------------------------+
// Get Reset Reason Hint requested by Application to enter UF2
//--------------------------------------------------------------------+

// copied from components/esp_system/port/soc/esp32s2/reset_reason.c
// since esp_system is not included with bootloader build
#define RST_REASON_BIT  0x80000000
#define RST_REASON_MASK 0x7FFF
#define RST_REASON_SHIFT 16

uint32_t /*IRAM_ATTR*/ esp_reset_reason_get_hint(void)
{
    uint32_t reset_reason_hint = REG_READ(RTC_RESET_CAUSE_REG);
    uint32_t high = (reset_reason_hint >> RST_REASON_SHIFT) & RST_REASON_MASK;
    uint32_t low = reset_reason_hint & RST_REASON_MASK;
    if ((reset_reason_hint & RST_REASON_BIT) == 0 || high != low) {
        return 0;
    }
    return low;
}

static void esp_reset_reason_clear_hint(void)
{
    REG_WRITE(RTC_RESET_CAUSE_REG, 0);
}

/*
 * We arrive here after the ROM bootloader finished loading this second stage bootloader from flash.
 * The hardware is mostly uninitialized, flash cache is down and the app CPU is in reset.
 * We do have a stack, so we can do the initialization in C.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    // (0. Call the before-init hook, if available)
    if (bootloader_before_init) {
        bootloader_before_init();
    }

    // 1. Hardware initialization
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    // (1.1 Call the after-init hook, if available)
    if (bootloader_after_init) {
        bootloader_after_init();
    }

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    // If this boot is a wake up from the deep sleep then go to the short way,
    // try to load the application which worked before deep sleep.
    // It skips a lot of checks due to it was done before (while first boot).
    bootloader_utility_load_boot_image_from_deep_sleep();
    // If it is not successful try to load an application as usual.
#endif

    // 2. Select the number of boot partition
    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

    // 3. Load the app image for booting
    bootloader_utility_load_boot_image(&bs, boot_index);
}

// Select the number of boot partition
static int select_partition_number(bootloader_state_t *bs)
{
    // 1. Load partition table
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    // 2. Select the number of boot partition
    return selected_boot_partition(bs);
}

/*
 * Selects a boot partition.
 * The conditions for switching to another firmware are checked.
 */
static int selected_boot_partition(const bootloader_state_t *bs)
{ 
    int boot_index = bootloader_utility_get_selected_boot_partition(bs);
    if (boot_index == INVALID_INDEX) {
        return boot_index; // Unrecoverable failure (not due to corrupt ota data or bad partition contents)
    }

    RESET_REASON reset_reason = bootloader_common_get_reset_reason(0);

    //ESP_LOGI(TAG, "selected_boot_partition stage 3 = %d", reset_reason);

    if (reset_reason != DEEPSLEEP_RESET)
    {
      if ( reset_reason == RTC_SW_SYS_RESET ||  reset_reason == RTC_SW_CPU_RESET )
      {
        //ESP_LOGI(TAG, "@@@@@@@@@@@@@@@@2 BOOT TO PROGRAM!!!");
        boot_index = 3;
      }
    }
    return boot_index;
}

// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}

//--------------------------------------------------------------------+
// Board LED Indicator
//--------------------------------------------------------------------+

static inline uint32_t ns2cycle(uint32_t ns)
{
  uint32_t tick_per_us;

#if CONFIG_IDF_TARGET_ESP32S3
  tick_per_us = ets_get_cpu_frequency();
#else // ESP32S2
  extern uint32_t g_ticks_per_us_pro;
  tick_per_us = g_ticks_per_us_pro;
#endif

  return (tick_per_us*ns) / 1000;
}

static inline uint32_t delay_cycle(uint32_t cycle)
{
  uint32_t ccount;
  uint32_t start = esp_cpu_get_ccount();
  while( (ccount = esp_cpu_get_ccount()) - start < cycle ) {}
  return ccount;
}


