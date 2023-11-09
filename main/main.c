/* Control with a touch pad playing MP3 files from SD Card

 This example code is in the Public Domain (or CC0 licensed, at your option.)

 Unless required by applicable law or agreed to in writing, this
 software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 CONDITIONS OF ANY KIND, either express or implied.
 */
#include <sd_card.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "periph_touch.h"
#include "periph_button.h"
#include "input_key_service.h"
#include "periph_adc_button.h"
#include "board.h"

#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
// #include "sdmmc_cmd.h"
// #include "driver/sdmmc_host.h"
// #include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "sdcard_list.h"
#include "sdcard_scan.h"
#include "sdkconfig.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"

#include "leds.h"

#include "stateConfig.h"

#include "audioPlayer.h"

#include "LAN.h"
#include "inttypes.h"
//#include "mdns.h"

#include "ftp.h"

#include "lwip/dns.h"

#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "argtable3/argtable3.h"
#include "esp_vfs_cdcacm.h"
#include "myMqtt.h"
#include "tusb.h"
#include "me_slot_config.h"
#include "executor.h"
#include "reporter.h"
#include "encoders.h"
#include "rfid.h"

#include "myCDC.h"

#include "p9813.h"

#include "tachometer.h"

#include "TOFs.h"

#include "stepper.h"


extern uint8_t SLOTS_PIN_MAP[6][4];

extern void board_init(void);



#define MDNS_ENABLE_DEBUG 0

static const char *TAG = "MAIN";

#define SPI_DMA_CHAN 1
// #define MOUNT_POINT "/sdcard"
// static const char *MOUNT_POINT = "/sdcard";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#define CONFIG_FREERTOS_HZ 1000

#define CFG_TUSB_DEBUG 3

int FTP_TASK_FINISH_BIT = BIT2;
EventGroupHandle_t xEventTask;

extern uint8_t FTP_SESSION_START_FLAG;
extern uint8_t FLAG_PC_AVAILEBLE;
extern uint8_t FLAG_PC_EJECT;

extern const char* VERSION;

extern void usb_device_task(void *param);

RTC_NOINIT_ATTR int RTC_flagMscEnabled;

extern exec_message_t exec_message;
extern QueueHandle_t exec_mailbox;

wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// static task
#define USBD_STACK_SIZE 4096
StackType_t usb_device_stack[USBD_STACK_SIZE];
StaticTask_t usb_device_taskdef;


#define CDC_STACK_SZIE      (configMINIMAL_STACK_SIZE*6)
StackType_t cdc_stack[CDC_STACK_SZIE];
StaticTask_t cdc_taskdef;
// static task for cdc
// #define CDC_STACK_SZIE      (configMINIMAL_STACK_SIZE*3)
// StackType_t cdc_stack[CDC_STACK_SZIE];
// StaticTask_t cdc_taskdef;
// void usb_device_task(void *param);
// void cdc_task(void *params);

// int isMscEnabled() {
//	return flagMscEnabled;
// }

#define RTC_FLAG_DISABLED_VALUE 0xAAAA5555

configuration me_config;
stateStruct me_state;

uint32_t ADC_AVERAGE;

#define RELAY_1_GPIO GPIO_NUM_18
#define RELAY_2_GPIO GPIO_NUM_48

void listenListener(void *pvParameters);

void ftp_task(void *pvParameters);

void nvs_init()
{
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();
	esp_err_t ret;
	ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	ESP_LOGD(TAG, "NVS init complite. Duration: %ld ms. Heap usage: %lu free heap:%u", (xTaskGetTickCount() - startTick) * portTICK_PERIOD_MS, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}


// void mdns_start(void)
// {
// 	//TO-DO mdns dont work whith ethernet
// 	uint32_t startTick = xTaskGetTickCount();
// 	uint32_t heapBefore = xPortGetFreeHeapSize();
// 	char mdnsName[80];

// 	// set mDNS hostname (required if you want to advertise services)
// 	if (strlen(me_config.device_name) == 0)
// 	{
// 		sprintf(mdnsName, "%s", (char *)me_config.ssidT);
// 		strcpy(mdnsName, me_config.ssidT);
// 		ESP_LOGD(TAG, "Set mdns name: %s  device_name len:%d ", mdnsName, strlen(me_config.device_name));
// 	}
// 	else
// 	{
// 		sprintf(mdnsName, "%s", me_config.device_name);
// 		ESP_LOGD(TAG, "Set mdns name: %s  device_name len:%d ", mdnsName, strlen(me_config.device_name));
// 	}

// 	ESP_ERROR_CHECK(mdns_init());
// 	ESP_ERROR_CHECK(mdns_hostname_set(mdnsName));
// 	ESP_ERROR_CHECK(mdns_instance_name_set("me-instance"));
// 	mdns_service_add(NULL, "_ftp", "_tcp", 21, NULL, 0);
// 	mdns_service_instance_name_set("_ftp", "_tcp", "me FTP server");
// 	strcat(mdnsName, ".local");
// 	ESP_LOGD(TAG, "mdns hostname set to: %s", mdnsName);
// 	mdns_txt_item_t serviceTxtData[1] = {
// 		{"URL", strdup(mdnsName)},
// 	};
// 	// sprintf()
// 	mdns_service_txt_set("_ftp", "_tcp", serviceTxtData, 1);

// 	ESP_LOGD(TAG, "mdns_start complite. Duration: %d ms. Heap usage: %lu free heap:%u", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
// }

void setLogLevel(uint8_t level)
{
	if (level == 3)
	{
		level = ESP_LOG_INFO;
	}
	else if (level == 4)
	{
		level = ESP_LOG_DEBUG;
	}
	else if (level == 2)
	{
		level = ESP_LOG_WARN;
	}
	else if (level == 1)
	{
		level = ESP_LOG_ERROR;
	}
	else if (level == 0)
	{
		level = ESP_LOG_NONE;
	}
	else if (level == 5)
	{
		level = ESP_LOG_VERBOSE;
	}

	esp_log_level_set("*", ESP_LOG_ERROR);
	esp_log_level_set("stateConfig", level);
	esp_log_level_set("console", level);
	esp_log_level_set("MAIN", level);
	esp_log_level_set(TAG, level);
	esp_log_level_set("AUDIO", level);
	esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_ERROR);
	esp_log_level_set("MP3_DECODER", ESP_LOG_ERROR);
	esp_log_level_set("CODEC_ELEMENT_HELPER:", ESP_LOG_ERROR);
	esp_log_level_set("FATFS_STREAM", ESP_LOG_ERROR);
	esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_ERROR);
	esp_log_level_set("I2S_STREAM", ESP_LOG_ERROR);
	esp_log_level_set("RSP_FILTER", ESP_LOG_ERROR);
	esp_log_level_set("WIFI", level);
	esp_log_level_set("esp_netif_handlers", level);
	esp_log_level_set("[Ftp]", level);
	esp_log_level_set("system_api", level);
	esp_log_level_set("MDNS", level);
	esp_log_level_set("[Ftp]", level);
	esp_log_level_set("mqtt", level);
	esp_log_level_set("leds", level);
	esp_log_level_set("ST7789", level);
	esp_log_level_set("JPED_Decoder", ESP_LOG_ERROR);
	esp_log_level_set("SDMMC", level);
	esp_log_level_set("ME_SLOT_CONFIG", level);
	esp_log_level_set("BUTTONS", level);
	esp_log_level_set("EXECUTOR", level);
	esp_log_level_set("REPORTER", level);
	esp_log_level_set("LAN", level);
	esp_log_level_set("3n_MOSFET", level);
	esp_log_level_set("RFID", level);
	esp_log_level_set("ENCODERS", level);
	esp_log_level_set("TOFs", level);
	esp_log_level_set("rotary_encoder", level);
	esp_log_level_set("P9813", level);
	esp_log_level_set("TACHOMETER", level);
	esp_log_level_set("ANALOG", level);
	esp_log_level_set("PN532", level);
	esp_log_level_set("STEPPER", level);
	esp_log_level_set("IN_OUT", level);
}



void heap_report(){
	while(1){
		ESP_LOGD(TAG, "free Heap size %d", xPortGetFreeHeapSize());
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

void app_main(void)
{

	setLogLevel(4);

	ESP_LOGD(TAG, "Start up");
	ESP_LOGD(TAG, "free Heap size %d", xPortGetFreeHeapSize());

	// initLeds();
	board_init(); // USB hardware

	nvs_init();

	
	xTaskCreateStatic(usb_device_task, "usbd", USBD_STACK_SIZE, NULL, configMAX_PRIORITIES - 1, usb_device_stack, &usb_device_taskdef);
	

	xTaskCreateStatic(cdc_task, "cdc", CDC_STACK_SZIE, NULL, configMAX_PRIORITIES - 2, cdc_stack, &cdc_taskdef);
	
	xTaskCreate(crosslinker_task, "cross_linker", 1024 * 4, NULL, configMAX_PRIORITIES - 8, NULL);
	//xTaskCreate(heap_report, "heap_report", 1024 * 4, NULL, configMAX_PRIORITIES - 8, NULL);
	

	exec_mailbox = xQueueCreate(10, sizeof(exec_message_t));
	if (exec_mailbox == NULL)
	{
		ESP_LOGE(TAG, "Exec_Mailbox create FAIL");
	}

	me_state.sd_init_res = ESP_FAIL;
	me_state.sd_init_res = spisd_init();
	if (me_state.sd_init_res != ESP_OK)	{
		ESP_LOGE(TAG, "sdcard_init FAIL");
		const char *base_path = "/sdcard";
		const esp_vfs_fat_mount_config_t mount_config = {
				.max_files = 4,
				.format_if_mount_failed = true,
				.allocation_unit_size = CONFIG_WL_SECTOR_SIZE
		};
		//esp_vfs_fat_spiflash_mount_rw_wl 
		//esp_err_t err = esp_vfs_fat_spiflash_mount(base_path, "storage", &mount_config, &s_wl_handle);
		esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(base_path, "storage", &mount_config, &s_wl_handle);
		
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
			return;
		}
	}
	if (remove("/sdcard/error.txt")){
		ESP_LOGD(TAG, "/sdcard/error.txt delete failed");
	}
	load_Default_Config();
	scanFileSystem();
	me_state.config_init_res = loadConfig();
	if (me_state.config_init_res != ESP_OK)	{
		char tmpString[40];
		sprintf(tmpString, "Load config FAIL in line: %d", me_state.config_init_res);
		writeErrorTxt(tmpString);
	}
	

	// gpio_config_t io_conf_p0 ={
	// 		.intr_type = GPIO_INTR_DISABLE,
	// 		.mode = GPIO_MODE_OUTPUT,
	// 		.pin_bit_mask = (1ULL << SLOTS_PIN_MAP[0][1]),
	// 		.pull_down_en = 0,
	// 		.pull_up_en = 1
	// 	};

	// gpio_config(&io_conf_p0);

	// gpio_set_level(SLOTS_PIN_MAP[0][1], 1);
	// vTaskDelay(1000);
	// gpio_set_level(SLOTS_PIN_MAP[0][1], 0);

	
	me_state.slot_init_res = init_slots();
	//start_benewakeTOF_task(0);

	//init_rfid_slot(0);
	
	if (strstr(me_config.slot_mode[0], "audio_player") != NULL) {
		me_state.content_search_res = loadContent();
		if (me_state.content_search_res != ESP_OK)	{
			ESP_LOGD(TAG, "Load Content FAIL");
			writeErrorTxt("Load content FAIL");
		}
	}else{
		me_state.content_search_res = ESP_FAIL;
	}

	if (me_config.LAN_enable == 1)	{
		LAN_init();
		
	}

	ESP_LOGI(TAG, "Ver %s. Load complite, start working. free Heap size %d", VERSION, xPortGetFreeHeapSize());

	//testStepper();
	startup_crosslinks_exec();
	
	while (1)
	{

		if (xQueueReceive(exec_mailbox, &exec_message, (25 / portTICK_PERIOD_MS)) == pdPASS)
		{
			ESP_LOGD(TAG, "Exec mail incoming:%s", exec_message.str);
			// char *event = exec_message.str + strlen(me_config.device_name) + 1;
			execute(exec_message.str);
		}

		vTaskDelay(pdMS_TO_TICKS(10));

	}

	
}

// USB Device Driver task
// This top level thread process all usb events and invoke callbacks
