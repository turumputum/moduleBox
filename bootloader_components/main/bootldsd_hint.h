#ifndef BOOTLDSD_HINT_H
#define BOOTLDSD_HINT_H

/* Подсказка прошивки загрузчику: "после перезагрузки запусти bootldsd, в /int лежит UPDATE.FW".
 *
 * Значение кладётся в RTC_CNTL_STORE6_REG (он же RTC_RESET_CAUSE_REG) через
 * esp_reset_reason_set_hint() - в том же формате, в котором IDF хранит причину
 * сброса: low15 | low15<<16 | 0x80000000. Регистр RTC-домена переживает
 * esp_restart(), bootloader читает его первым и тут же обнуляет.
 *
 * Один и тот же заголовок включают:
 *   - bootloader_components/main/bootloader_start.c (читает регистр)
 *   - main/main.c requestFirmwareLoader()            (пишет через esp_reset_reason_set_hint)
 * Значение 0x11F2 - из TinyUF2 (APP_REQUEST_UF2_RESET_HINT), 15 бит, не пересекается
 * с esp_reset_reason_t (ESP_RST_* <= 0x14). */
#define BOOTLDSD_RESET_HINT        0x11F2u
#define BOOTLDSD_RESET_HINT_REGVAL (BOOTLDSD_RESET_HINT | (BOOTLDSD_RESET_HINT << 16) | 0x80000000u)

#endif
