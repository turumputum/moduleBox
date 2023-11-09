
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

#include "esp_log.h"
#include "sdkconfig.h"
//#define FORCE_SD_40MHZ

//esp_err_t sdmmc_host_set_card_clk(int slot, uint32_t freq_khz);

static const char *TAG = "SDMMC";

#define MOUNT_POINT "/sdcard"
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

int spisd_mount_fs() {
	int result = -1;
	esp_err_t ret;
	const char mount_point[] = MOUNT_POINT;

	ESP_LOGD(TAG, "Mounting filesystem");
	//ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
	ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config,&mount_config, &card);
	if (ret == ESP_OK) {
		ESP_LOGD(TAG, "Filesystem mounted:");
		//sdmmc_card_print_info(stdout, card);

		result = ESP_OK;
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

	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();
	int result = -1;
	esp_err_t ret;

	ESP_LOGD(TAG, "Initializing SD card");

#ifdef FORCE_SD_40MHZ
    host.max_freq_khz  = 40000;
#endif
	gpio_pad_select_gpio(48);
	gpio_set_direction(48, GPIO_MODE_OUTPUT);
	gpio_set_level(48, 1);

	slot_config.clk = 47;
	slot_config.cmd = 21;
	slot_config.d0 = 40;
	// slot_config.d1 = 38;
	// slot_config.d2 = 39;
	// slot_config.d3 = 40;
	slot_config.width = 1;

	int res=spisd_mount_fs();
	ESP_LOGD(TAG, "SDcard init complite. Duration: %ld ms. Heap usage: %lu free Heap:%u", (xTaskGetTickCount() - startTick) * portTICK_PERIOD_MS, heapBefore - xPortGetFreeHeapSize(),
				xPortGetFreeHeapSize());

	if(res==ESP_OK){
		gpio_set_level(48, 0);
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

	if (sdmmc_read_sectors(card, dst, start_sector, num) == ESP_OK) {
		result = 1;
	}

	return result;
}
int spisd_sectors_write(void *dst, uint32_t start_sector, uint32_t num) {
	int result = -1;

	if (sdmmc_write_sectors(card, dst, start_sector, num) == ESP_OK) {
		result = 1;
	}

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

