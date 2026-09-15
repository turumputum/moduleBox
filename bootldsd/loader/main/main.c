/*
 * bootldsd (loader) - загрузчик обновлений moduleBox.
 *
 * Живёт в разделе bootldsd (ota_0 @ 0x10000). Кастомный bootloader (bootloader_components/main
 * в основном проекте) запускает его после аппаратного сброса или по RTC-подсказке от прошивки.
 *
 * Работа:
 *   1) монтирует раздел storage (FAT + wear levelling) как /int - туда прошивка кладёт
 *      UPDATE.FW, полученный по FTP (main/main.c moveUpdateToTheInternalStorage);
 *   2) проверяет заголовок (magic, размер) и CRC32 полезной нагрузки;
 *   3) стирает раздел program (factory @ 0x50000), пишет образ, перечитывает и сверяет CRC;
 *   4) удаляет UPDATE.FW и делает esp_restart() - программный сброс ведёт в program.
 *
 * Битый файл (magic/размер/CRC) удаляется, чтобы не проверять его на каждом старте.
 * Ошибка записи во flash файл НЕ удаляет: следующий аппаратный сброс повторит попытку,
 * а bootloader при негодном program сам откатится на bootldsd.
 * Карта памяти не нужна и не трогается - пины платы для загрузчика не важны.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_vfs_fat.h"
#include "esp_rom_crc.h"
#include "wear_levelling.h"
#include "sdkconfig.h"
#include "update.h"

#define MOUNT_POINT     "/int"
#define STORAGE_LABEL   "storage"
#define UPDATE_PATH     MOUNT_POINT "/" UPDATE_FILENAME
#define IO_CHUNK        (32 * 1024)
#define FLASH_SECTOR    4096
#define ESP_IMAGE_MAGIC 0xE9

static const char *TAG = "bootldsd";

typedef enum {
    UPD_NONE = 0,       /* файла нет */
    UPD_BAD_FILE,       /* файл негодный - удалить */
    UPD_FLASH_FAILED,   /* запись/сверка не удалась - оставить файл на повтор */
    UPD_APPLIED,        /* прошито и сверено */
} upd_result_t;

/* CRC32 полезной нагрузки файла от текущей позиции. esp_rom_crc32_le(0, ...) по цепочке
   даёт стандартный CRC32 (init/xorout 0xFFFFFFFF) - ровно то, что считает bin2fw.c */
static uint32_t file_payload_crc(FILE *fp, uint8_t *buf)
{
    uint32_t crc = 0;
    size_t rd;
    while ((rd = fread(buf, 1, IO_CHUNK, fp)) > 0) {
        crc = esp_rom_crc32_le(crc, buf, rd);
    }
    return crc;
}

static uint32_t partition_crc(const esp_partition_t *part, uint32_t size, uint8_t *buf)
{
    uint32_t crc = 0;
    for (uint32_t off = 0; off < size; ) {
        uint32_t n = size - off;
        if (n > IO_CHUNK) n = IO_CHUNK;
        if (esp_partition_read(part, off, buf, n) != ESP_OK) {
            ESP_LOGE(TAG, "read back failed at 0x%lx", (unsigned long)off);
            return ~crc; /* заведомо не совпадёт */
        }
        crc = esp_rom_crc32_le(crc, buf, n);
        off += n;
    }
    return crc;
}

static upd_result_t apply_update(FILE *fp, const UPDATEHEAD *hdr, const esp_partition_t *part, uint8_t *buf)
{
    uint32_t erase_len = (hdr->size + FLASH_SECTOR - 1) & ~(uint32_t)(FLASH_SECTOR - 1);

    ESP_LOGI(TAG, "erasing '%s' (0x%lx, %lu bytes)", part->label,
             (unsigned long)part->address, (unsigned long)erase_len);
    esp_err_t err = esp_partition_erase_range(part, 0, erase_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
        return UPD_FLASH_FAILED;
    }

    ESP_LOGI(TAG, "writing %lu bytes", (unsigned long)hdr->size);
    fseek(fp, sizeof(UPDATEHEAD), SEEK_SET);
    uint32_t written = 0;
    size_t rd;
    while ((rd = fread(buf, 1, IO_CHUNK, fp)) > 0) {
        if (written + rd > hdr->size) {
            rd = hdr->size - written;
        }
        err = esp_partition_write(part, written, buf, rd);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write failed at 0x%lx: %s", (unsigned long)written, esp_err_to_name(err));
            return UPD_FLASH_FAILED;
        }
        written += rd;
        if (written >= hdr->size) break;
    }
    if (written != hdr->size) {
        ESP_LOGE(TAG, "short read from file: %lu of %lu", (unsigned long)written, (unsigned long)hdr->size);
        return UPD_FLASH_FAILED;
    }

    uint32_t crc = partition_crc(part, hdr->size, buf);
    if (crc != hdr->checksum) {
        ESP_LOGE(TAG, "verify failed: flash crc %08lx, expected %08lx",
                 (unsigned long)crc, (unsigned long)hdr->checksum);
        return UPD_FLASH_FAILED;
    }

    ESP_LOGI(TAG, "update applied and verified");
    return UPD_APPLIED;
}

static upd_result_t process_update(void)
{
    FILE *fp = fopen(UPDATE_PATH, "rb");
    if (fp == NULL) {
        ESP_LOGI(TAG, "no %s", UPDATE_PATH);
        return UPD_NONE;
    }

    upd_result_t result = UPD_BAD_FILE;
    uint8_t *buf = malloc(IO_CHUNK);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory for io buffer");
        fclose(fp);
        return UPD_FLASH_FAILED;
    }

    struct stat st;
    UPDATEHEAD hdr;
    if (fstat(fileno(fp), &st) != 0 || st.st_size < (off_t)sizeof(UPDATEHEAD)) {
        ESP_LOGE(TAG, "update file too short");
        goto done;
    }
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1) {
        ESP_LOGE(TAG, "cannot read header");
        goto done;
    }
    if (hdr.magic != UPDATE_MAGIC) {
        ESP_LOGE(TAG, "bad magic %08lx", (unsigned long)hdr.magic);
        goto done;
    }
    if ((uint32_t)st.st_size - sizeof(UPDATEHEAD) != hdr.size || hdr.size < FLASH_SECTOR) {
        ESP_LOGE(TAG, "size mismatch: header %lu, file payload %lu",
                 (unsigned long)hdr.size, (unsigned long)(st.st_size - sizeof(UPDATEHEAD)));
        goto done;
    }

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                           ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    const esp_partition_t *self = esp_ota_get_running_partition();
    if (part == NULL || (self != NULL && part->address == self->address)) {
        ESP_LOGE(TAG, "target factory partition not found or is bootldsd itself");
        result = UPD_FLASH_FAILED;
        goto done;
    }
    if (hdr.size > part->size) {
        ESP_LOGE(TAG, "image %lu bytes does not fit partition '%s' (%lu)",
                 (unsigned long)hdr.size, part->label, (unsigned long)part->size);
        goto done;
    }

    int first = fgetc(fp);
    if (first != ESP_IMAGE_MAGIC) {
        ESP_LOGE(TAG, "payload is not an esp image (first byte 0x%02x)", first);
        goto done;
    }

    ESP_LOGI(TAG, "checking %s (%lu bytes)", UPDATE_PATH, (unsigned long)hdr.size);
    fseek(fp, sizeof(UPDATEHEAD), SEEK_SET);
    uint32_t crc = file_payload_crc(fp, buf);
    if (crc != hdr.checksum) {
        ESP_LOGE(TAG, "crc mismatch: file %08lx, header %08lx", (unsigned long)crc, (unsigned long)hdr.checksum);
        goto done;
    }

    result = apply_update(fp, &hdr, part, buf);

done:
    fclose(fp);
    free(buf);
    if (result == UPD_BAD_FILE || result == UPD_APPLIED) {
        if (unlink(UPDATE_PATH) == 0) {
            ESP_LOGI(TAG, "%s removed", UPDATE_PATH);
        } else {
            ESP_LOGW(TAG, "cannot remove %s", UPDATE_PATH);
        }
    }
    return result;
}

void app_main(void)
{
    ESP_LOGI(TAG, "moduleBox firmware loader, built " __DATE__ " " __TIME__);

    wl_handle_t wl = WL_INVALID_HANDLE;
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 2,
        .format_if_mount_failed = false,   /* форматирует только прошивка; тут нечего терять, но и нечего чинить */
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, STORAGE_LABEL, &cfg, &wl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cannot mount '%s' as %s: %s - nothing to do", STORAGE_LABEL, MOUNT_POINT, esp_err_to_name(err));
    } else {
        upd_result_t r = process_update();
        ESP_LOGI(TAG, "result: %s",
                 r == UPD_APPLIED ? "updated" :
                 r == UPD_NONE ? "no update" :
                 r == UPD_BAD_FILE ? "bad update file, removed" : "flash failed, file kept for retry");
        esp_vfs_fat_spiflash_unmount_rw_wl(MOUNT_POINT, wl);
    }

    ESP_LOGI(TAG, "restarting into program");
    esp_restart();
}
