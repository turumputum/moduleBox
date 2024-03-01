#include <aip.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_vfs_fat.h"
#include "esp_spiffs.h"

wl_handle_t s_wl_handle = WL_INVALID_HANDLE;


uint8_t scan_dir(const char *path) {
    FRESULT res;
    FF_DIR dir;
    FILINFO fno;
    uint8_t picIndex = 0;
    uint8_t soundIndex = 0;

    res = f_opendir(&dir, path);
    if (res == FR_OK) {
        while (1) {
            res = f_readdir(&dir, &fno); /* Read a directory item */

            if ((res == FR_OK) && (fno.fname[0] != 0)) {

                // if (fno.fattrib & AM_DIR)
                //     printf("  [%s]\n", fno.fname);
                // else
                //     printf("  %s (%d Kb)\n", fno.fname, (unsigned)fno.fsize / 1024);

                if (!(fno.fattrib & AM_HID)) {
                    if (fno.fattrib & AM_DIR) {
                        //scan_dir(fno.fname);
                    } else {
                    }
                }
            } else {
                break;
            }
        }
    }
    return soundIndex;
}

int aip_init()
{
    // esp_vfs_spiffs_conf_t conf = {
    //     .base_path = MOUNT_POINT,
    //     .partition_label = NULL,
    //     .max_files = 5,
    //     .format_if_mount_failed = false};

    // return esp_vfs_spiffs_register(&conf) == ESP_OK;

    const char *base_path = MOUNT_POINT;
    const esp_vfs_fat_mount_config_t mount_config = {
            .max_files = 2,
            .format_if_mount_failed = true,
            .allocation_unit_size = CONFIG_WL_SECTOR_SIZE
    };
    //esp_vfs_fat_spiflash_mount_rw_wl 
    //esp_err_t err = esp_vfs_fat_spiflash_mount(base_path, "storage", &mount_config, &s_wl_handle);
    esp_err_t err = esp_vfs_fat_spiflash_mount(base_path, "storage", &mount_config, &s_wl_handle);

    if (err != ESP_OK) {
        printf("Failed to mount FS (%s)\n", esp_err_to_name(err));
    }
    else
    {
        printf("FS is mounted\n");

        //scan_dir("/");
    }

    return (err == ESP_OK);
}
