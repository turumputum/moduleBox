/*
 * Второй загрузчик moduleBox - переопределение штатного bootloader_start.c ESP-IDF v5.5.4
 * (механизм bootloader_components/main). Отличие от штатного - только selected_boot_partition().
 *
 * Таблица разделов (partitions.csv):
 *   bootldsd  app/ota_0    0x10000  загрузчик обновлений: /int/UPDATE.FW -> program
 *   program   app/factory  0x50000  основная прошивка
 *
 * Правило выбора:
 *   1) в RTC_CNTL_STORE6_REG лежит подсказка BOOTLDSD_RESET_HINT (её ставит прошивка
 *      после переноса UPDATE.FW в /int, см. main/main.c requestFirmwareLoader) -> bootldsd,
 *      подсказка тут же обнуляется, чтобы не зациклиться;
 *   2) сброс не программный (power-on, EN, brownout, RTC WDT, USB-serial) -> bootldsd:
 *      он проверит /int и, если файла нет, за доли секунды перезапустится в program;
 *   3) программный сброс (esp_restart, panic) -> program.
 *      Так bootldsd после своей работы всегда попадает в program, а падение прошивки
 *      не гоняет её через загрузчик.
 * Если раздела bootldsd в таблице нет или он пуст - штатная логика (factory = program).
 */
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_rom_caps.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/reset_reasons.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "bootloader_hooks.h"
#include "bootldsd_hint.h"

static const char *TAG = "boot";

/* Индекс ota_0 в bootloader_state_t::ota[] - там лежит раздел bootldsd */
#define BOOTLDSD_OTA_INDEX 0

static int select_partition_number(bootloader_state_t *bs);
static int selected_boot_partition(const bootloader_state_t *bs);

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

#if CONFIG_SECURE_ENABLE_TEE
    bootloader_utility_load_tee_image(&bs);
#endif

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

static bool reset_is_software(soc_reset_reason_t rr)
{
    return rr == RESET_REASON_CORE_SW || rr == RESET_REASON_CPU0_SW;
}

/*
 * Selects a boot partition.
 * The conditions for switching to another firmware are checked.
 */
static int selected_boot_partition(const bootloader_state_t *bs)
{
    /* Штатный выбор: otadata нет -> factory (program). Заодно проверка таблицы. */
    int boot_index = bootloader_utility_get_selected_boot_partition(bs);
    if (boot_index == INVALID_INDEX) {
        return boot_index; // Unrecoverable failure (not due to corrupt ota data or bad partition contents)
    }

    soc_reset_reason_t rr = esp_rom_get_reset_reason(0);
    if (rr == RESET_REASON_CORE_DEEP_SLEEP) {
        return boot_index;
    }

    if (bs->ota[BOOTLDSD_OTA_INDEX].size == 0) {
        ESP_LOGW(TAG, "no bootldsd partition (ota_0), booting program");
        return boot_index;
    }

    uint32_t hint = REG_READ(RTC_CNTL_STORE6_REG);
    if (hint == BOOTLDSD_RESET_HINT_REGVAL) {
        REG_WRITE(RTC_CNTL_STORE6_REG, 0);
        ESP_LOGI(TAG, "firmware asked for update: booting bootldsd");
        return BOOTLDSD_OTA_INDEX;
    }

    if (!reset_is_software(rr)) {
        ESP_LOGI(TAG, "hard reset (reason %d): booting bootldsd", (int)rr);
        return BOOTLDSD_OTA_INDEX;
    }

    return boot_index;
}

// Return global reent struct if any newlib functions are linked to bootloader
#if CONFIG_LIBC_NEWLIB
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
