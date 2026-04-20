// Дефайны для сдКарты
// #define PIN_NUM_MISO 6
// #define PIN_NUM_MOSI 7
// #define PIN_NUM_CLK 15
// #define PIN_NUM_CS 16

#include "bsp/board.h"
#include "rom/gpio.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/semphr.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"

#include <mbdebug.h>


#include "esp_log.h"
#include "sdkconfig.h"
//#define FORCE_SD_40MHZ

//esp_err_t sdmmc_host_set_card_clk(int slot, uint32_t freq_khz);

static const char *TAG = "SDMMC";

#define MOUNT_POINT "/sdcard"

static SemaphoreHandle_t s_sdcard_mutex = NULL;

void sdcard_lock(void) {
	if (s_sdcard_mutex) xSemaphoreTake(s_sdcard_mutex, portMAX_DELAY);
}

void sdcard_unlock(void) {
	if (s_sdcard_mutex) xSemaphoreGive(s_sdcard_mutex);
}

// ---------- Mutex-wrapped diskio для FATFS ----------
// Регистрируем свои функции после mount, чтобы и FATFS (аудио),
// и USB MSC (прямые sector read/write) использовали один мьютекс.
// Без этого FATFS и USB MSC конкурируют на SDMMC между CMD18 и CMD12.

// forward declaration — определение ниже в файле
extern sdmmc_card_t *card;

static DSTATUS sd_diskio_init(BYTE pdrv) { return 0; }
static DSTATUS sd_diskio_status(BYTE pdrv) { return 0; }

static DRESULT sd_diskio_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
	sdcard_lock();
	esp_err_t err = sdmmc_read_sectors(card, buff, sector, count);
	sdcard_unlock();
	return (err == ESP_OK) ? RES_OK : RES_ERROR;
}

static DRESULT sd_diskio_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
	sdcard_lock();
	esp_err_t err = sdmmc_write_sectors(card, (void*)buff, sector, count);
	sdcard_unlock();
	return (err == ESP_OK) ? RES_OK : RES_ERROR;
}

static DRESULT sd_diskio_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
	switch (cmd) {
		case CTRL_SYNC:       return RES_OK;
		case GET_SECTOR_COUNT: *(LBA_t*)buff = card->csd.capacity;    return RES_OK;
		case GET_SECTOR_SIZE:  *(WORD*)buff  = card->csd.sector_size; return RES_OK;
		case GET_BLOCK_SIZE:   *(DWORD*)buff = 1;                     return RES_OK;
		default:               return RES_PARERR;
	}
}

static const ff_diskio_impl_t s_mutex_diskio = {
	.init   = sd_diskio_init,
	.status = sd_diskio_status,
	.read   = sd_diskio_read,
	.write  = sd_diskio_write,
	.ioctl  = sd_diskio_ioctl,
};
// -------------------------------------------------------
#define SPI_DMA_CHAN    SPI_DMA_CH_AUTO
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG


#define SOC_SDMMC_USE_GPIO_MATRIX 1

sdmmc_host_t host = SDMMC_HOST_DEFAULT();
///sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t *card;
sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
//sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();

esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
		.format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
		.max_files = 4, .allocation_unit_size = 16 * 1024 };

int spisd_deinit() {
	return 1;
}

void spisd_umount_fs()
{
	const char mount_point[] = MOUNT_POINT;

	esp_vfs_fat_sdcard_unmount(mount_point, card);
}

int spisd_mount_fs() {
	int result = -1;
	esp_err_t ret;
	const char mount_point[] = MOUNT_POINT;

	ESP_LOGD(TAG, "Mounting filesystem");
	//ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
	ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config,&mount_config, &card);
	if (ret == ESP_OK) {
		//ESP_LOGD(TAG, "Filesystem mounted:");
		//sdmmc_card_print_info(stdout, card);

		// Log filesystem information
		uint64_t total_bytes = 0, free_bytes = 0;
		if (esp_vfs_fat_info(MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK) {
			ESP_LOGI(TAG, "Filesystem type: FAT");
			ESP_LOGI(TAG, "CSD Ver: %d", card->csd.csd_ver);
			ESP_LOGI(TAG, "MMC Ver: %d", card->csd.mmc_ver);
			ESP_LOGI(TAG, "Capacity: %d", card->csd.capacity);
			ESP_LOGI(TAG, "Sector Size: %d", card->csd.sector_size);
			ESP_LOGI(TAG, "Read Block Len: %d", card->csd.read_block_len);
			ESP_LOGI(TAG, "Card Command Class: %d", card->csd.card_command_class);
			ESP_LOGI(TAG, "TR Speed: %d", card->csd.tr_speed);
			ESP_LOGI(TAG, "Total size: %llu MB", total_bytes / (1024 * 1024));
			ESP_LOGI(TAG, "Free size: %llu MB", free_bytes / (1024 * 1024));
		} else {
			ESP_LOGE(TAG, "Failed to get filesystem info");
		}

		result = ESP_OK;

		// Заменяем стандартный diskio FATFS на наш, защищённый мьютексом.
		// Это предотвращает конкуренцию FATFS (аудио) и USB MSC за SDMMC шину.
		// pdrv=0 — первый (и единственный) примонтированный накопитель.
		BYTE pdrv = ff_diskio_get_pdrv_card(card);
		if (pdrv != 0xFF) {
			ff_diskio_register(pdrv, &s_mutex_diskio);
			ESP_LOGD(TAG, "Mutex diskio registered for pdrv=%d", pdrv);
		} else {
			ESP_LOGW(TAG, "ff_diskio_get_pdrv_card failed, using pdrv=0");
			ff_diskio_register(0, &s_mutex_diskio);
		}
	} else {
		if (ret == ESP_FAIL) {
			ESP_LOGI(TAG,
					"Failed to mount filesystem. " "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.\n");
		} else {
			ESP_LOGI(TAG,
					"Failed to initialize the card (%s). " "Make sure SD card lines have pull-up resistors in place.\n",
					esp_err_to_name(ret));
		}
	}

	return result;
}

int spisd_init() {

	if (!s_sdcard_mutex) {
		s_sdcard_mutex = xSemaphoreCreateMutex();
	}

	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int result = -1;
	esp_err_t ret;

	ESP_LOGD(TAG, "Initializing SD card");

	extern configuration me_config;
	uint8_t clk_pin, cmd_pin, d0_pin, led_pin;

#ifdef BOARD_PINOUT_V6
	clk_pin = 8; cmd_pin = 9; d0_pin = 7; led_pin = 0;
#else
	// Два набора пинов: v3 (old), v4 (new)
	const uint8_t pin_sets[][4] = {
		// {clk, cmd, d0, led}
		{47, 21, 40, 48},  // v3
		{41, 40,  3, 48},  // v4
	};
	int set_order[2];

	if(me_config.boardVersion == 4){
		set_order[0] = 1; set_order[1] = 0;  // v4 → v3
	}else{
		set_order[0] = 0; set_order[1] = 1;  // v3 → v4
	}

	int found = 0;
	for(int s = 0; s < 2; s++){
		int idx = set_order[s];
		clk_pin = pin_sets[idx][0];
		cmd_pin = pin_sets[idx][1];
		d0_pin  = pin_sets[idx][2];
		led_pin = pin_sets[idx][3];

		gpio_pad_select_gpio(clk_pin);
		gpio_set_direction(clk_pin, GPIO_MODE_INPUT);
		gpio_pad_select_gpio(cmd_pin);
		gpio_set_direction(cmd_pin, GPIO_MODE_INPUT);
		gpio_pad_select_gpio(d0_pin);
		gpio_set_direction(d0_pin, GPIO_MODE_INPUT);

		if((gpio_get_level(clk_pin)==1)&&(gpio_get_level(d0_pin)==1)&&(gpio_get_level(cmd_pin)==1)){
			ESP_LOGD(TAG, "SD card found on pin set %d (clk=%d cmd=%d d0=%d)", idx, clk_pin, cmd_pin, d0_pin);
			found = 1;
			break;
		}
		ESP_LOGD(TAG, "Pin set %d (clk=%d cmd=%d d0=%d) notFound", idx, clk_pin, cmd_pin, d0_pin);
	}

	if(!found){
		ESP_LOGW(TAG, "SD card module notFound(");
		return ESP_FAIL;
	}
#endif

	gpio_pad_select_gpio(led_pin);
	gpio_set_direction(led_pin, GPIO_MODE_OUTPUT);
	gpio_set_level(led_pin, 1);

	gpio_set_pull_mode(clk_pin, GPIO_PULLUP_ONLY);
	gpio_set_pull_mode(cmd_pin, GPIO_PULLUP_ONLY);
	gpio_set_pull_mode(d0_pin, GPIO_PULLUP_ONLY);

	slot_config.clk = clk_pin;
	slot_config.cmd = cmd_pin;
	slot_config.d0 = d0_pin;
	slot_config.width = 1;

	// SDMMC_FREQ_DEFAULT = 20MHz — стандартная скорость для GPIO Matrix.
	// 40MHz (HIGHSPEED) через GPIO Matrix + USB-нагрузка даёт end-bit error (0x8008).
	// input_delay_phase работает только при HIGHSPEED/52M, при 20MHz не нужна.
	host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
	host.input_delay_phase = SDMMC_DELAY_PHASE_1;

	int res=spisd_mount_fs();
	ESP_LOGD(TAG, "SDcard init complite. Duration: %ld ms. Heap usage: %lu free Heap:%u", (xTaskGetTickCount() - startTick) * portTICK_PERIOD_MS, heapBefore - xPortGetFreeHeapSize(),
				xPortGetFreeHeapSize());

	if(res==ESP_OK){
		gpio_set_level(led_pin, 0);
	}
	return res;
}

int spisd_get_sector_size() {
	return card->csd.sector_size;
}
int spisd_get_sector_num() {
	return card->csd.capacity;
}

int spisd_sectors_read(void *dst, uint32_t start_sector, uint32_t num) {
	int result = -1;
	sdcard_lock();
    esp_err_t ret = sdmmc_read_sectors(card, dst, start_sector, num);
	sdcard_unlock();
	if (ret == ESP_OK) {
		result = 1;
	} else {
		ESP_LOGE(TAG, "sdmmc_read_sectors failed: 0x%x", ret);
		// if (ret == 0x107) { // ESP_ERR_TIMEOUT
		// 	ESP_LOGE(TAG, "Read timeout detected. Attempting to format card...");
			
		// 	esp_err_t format_ret = esp_vfs_fat_sdcard_format(MOUNT_POINT, card);
			
		// 	if (format_ret != ESP_OK) {
		// 		ESP_LOGE(TAG, "Failed to format card (%s)", esp_err_to_name(format_ret));
        //         // If format failed with "resources recycled", unmount was likely handled internally or resources freed.
        //         // We should just clean our pointer and try to mount again.
        //         card = NULL;

        //         ESP_LOGI(TAG, "Attempting to remount after failed format...");
        //         spisd_mount_fs();
		// 	} else {
		// 		ESP_LOGI(TAG, "Card formatted successfully.");
                
        //         // Unmount to ensure clean state before remounting
        //         // Check if card is still valid before unmounting?
        //         // Assuming success format kept card valid or we need to unmount VFS.
		// 	    if (card) {
        //             esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
        //             card = NULL;
        //         }

		// 	    ESP_LOGI(TAG, "Remounting...");
        //         spisd_mount_fs();
		// 	}
		// }
	}

	return result;
}

int spisd_sectors_write(void *dst, uint32_t start_sector, uint32_t num) {
	int result = -1;

	sdcard_lock();
	if (sdmmc_write_sectors(card, dst, start_sector, num) == ESP_OK) {
		result = 1;
	}
	sdcard_unlock();

	return result;
}

void usbprintf(char *msg, ...);

void spisd_list_root() {
	struct dirent *entry;
	struct stat entry_stat;

	DIR *dir = opendir(MOUNT_POINT);

	if (dir != NULL) {
		usbprintf("\n\nRoot dir list:\n\n");

		while ((entry = readdir(dir)) != NULL) {
			usbprintf("%s%s%s\n", entry->d_type == DT_DIR ? "[" : "",
					entry->d_name, entry->d_type == DT_DIR ? "]" : "");
		}
		closedir(dir);
	} else
		usbprintf("cannot open dir!\n");
}

