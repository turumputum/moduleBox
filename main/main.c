/* Control with a touch pad playing MP3 files from SD Card

 This example code is in the Public Domain (or CC0 licensed, at your option.)

 Unless required by applicable law or agreed to in writing, this
 software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 CONDITIONS OF ANY KIND, either express or implied.
 */
#include <sd_card.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/FreeRTOSConfig.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_freertos_hooks.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "periph_touch.h"
#include "periph_button.h"
#include "input_key_service.h"
#include "periph_adc_button.h"
#include "board.h"
#include "udplink.h"

// #include "rtp_play.h"
#include <stdio.h>
#include <stdlib.h>
#include "esp_task_wdt.h"
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

#include "stateConfig.h"

#include "audioPlayer.h"

#include "LAN.h"
#include "inttypes.h"
#include "WIFI.h"

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
#include "in_out.h"


#include "myCDC.h"

#include "p9813.h"

#include "3n_mosfet.h"
#include "swiper.h"
//#include "smartLed.h"
#include "someUnique.h"

#include <manifest.h>
#include <mbdebug.h>
#include <moduleboxapp.h>

#include <scheduler.h>





extern uint8_t SLOTS_PIN_MAP[10][4];

extern void board_init(void);



#define MDNS_ENABLE_DEBUG 0

static const char *TAG = "MAIN";

#define SPI_DMA_CHAN 1
// #define MOUNT_POINT "/sdcard"
// static const char *MOUNT_POINT = "/sdcard";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#undef  LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#define CONFIG_FREERTOS_HZ 1000

#undef  CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 3


int FTP_TASK_FINISH_BIT = BIT2;
EventGroupHandle_t xEventTask;



extern uint8_t FTP_SESSION_START_FLAG;
extern uint8_t FLAG_PC_AVAILEBLE;
extern uint8_t FLAG_PC_EJECT;

extern void usb_device_task(void *param);
extern void set_usb_debug(void);

//RTC_NOINIT_ATTR int RTC_flagMscEnabled;

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


void setLogLevel(uint8_t level){
	if (level == 3){
		level = ESP_LOG_INFO;
	}
	else if (level == 4){
		level = ESP_LOG_DEBUG;
	}
	else if (level == 2){
		level = ESP_LOG_WARN;
	}
	else if (level == 1){
		level = ESP_LOG_ERROR;
	}
	else if (level == 0){
		level = ESP_LOG_NONE;
	}
	else if (level == 5){
		level = ESP_LOG_VERBOSE;
	}

	esp_log_level_set("*", ESP_LOG_ERROR);
	esp_log_level_set("stateConfig", level);
	esp_log_level_set("console", level);
	esp_log_level_set("MAIN", level);
	esp_log_level_set(TAG, level);
	esp_log_level_set("AUDIO", level);
	esp_log_level_set("WAV", level);
	esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_ERROR);
	esp_log_level_set("MP3_DECODER", ESP_LOG_ERROR);
	esp_log_level_set("CODEC_ELEMENT_HELPER:", ESP_LOG_ERROR);
	esp_log_level_set("FATFS_STREAM", ESP_LOG_ERROR);
	esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_ERROR);
	esp_log_level_set("I2S_STREAM", ESP_LOG_ERROR);
	esp_log_level_set("RSP_FILTER", ESP_LOG_ERROR);
	esp_log_level_set("WIFI", level);
	esp_log_level_set("esp_netif_handlers", level);
	esp_log_level_set("[FTP]", level);
	esp_log_level_set("system_api", level);
	esp_log_level_set("MDNS", level);
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
	esp_log_level_set("[UDP]", level);
	esp_log_level_set("3n_MOSFET", level);
	esp_log_level_set("RFID", level);
	esp_log_level_set("ENCODERS", level);
	esp_log_level_set("DISTANCE_SENS", level);
	esp_log_level_set("rotary_encoder", level);
	esp_log_level_set("P9813", level);
	esp_log_level_set("TACHOMETER", level);
	esp_log_level_set("ANALOG", level);
	esp_log_level_set("ADC1", level);
	esp_log_level_set("PN532", level);
	esp_log_level_set("STEPPER", level);
	esp_log_level_set("IN_OUT", level);
	esp_log_level_set("SMART_LED", level);
	esp_log_level_set("myCDC", level);
	esp_log_level_set("SENSOR_2CH", level);
	esp_log_level_set("TENZO_BUTTON", level);
	esp_log_level_set("HID_BUTTON", level);
	esp_log_level_set("USB", level);
	esp_log_level_set("FLYWHEEL", level);
	esp_log_level_set("VIRTUAL_SLOTS", level);
	esp_log_level_set("myHID", level);
	esp_log_level_set("DWIN_UART", level);
	esp_log_level_set("SWIPER", level);
	esp_log_level_set("SOME_UNIQUE", level);
	esp_log_level_set("AUDIO_LAN", level);
	esp_log_level_set("RTP_STREAM", level);
	esp_log_level_set("ONE_WIRE", level);
	esp_log_level_set("ACCEL", level);
	esp_log_level_set("STEADYWIN", level);
	esp_log_level_set("VESC", level);
	esp_log_level_set("PPM", level);
	esp_log_level_set("CRSF", level);
	esp_log_level_set("RGB|HSV", level);
	esp_log_level_set("rmt", level);
	esp_log_level_set("servoDev", level);
	esp_log_level_set("CyberGear", level);
	esp_log_level_set("BUTTON_LEDS", level);
	}


void heap_report(){
	while(1){
		ESP_LOGD(TAG, "free Heap size %d", xPortGetFreeHeapSize());
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}


bool startNetworkServices()
{
	bool result 			= false;
	char errorString		[40] = "";

	if (me_config.LAN_enable || me_config.WIFI_enable)
	{
		if (me_config.LAN_enable == 1)	
		{
			if (LAN_init() != 0)
			{
				sprintf(errorString, "%s", "LAN init failed");
			}
			else
				result = true;
		}
		if (me_config.WIFI_enable == 1)
		{
			if (wifiInit() != ESP_OK)
			{
				sprintf(errorString, "%s", "WIFI init failed");
			}
			else
				result = true;
		}

		if (result)
		{
#define NETWORK_INIT_TIMEOUT		100

			int timeout		= NETWORK_INIT_TIMEOUT;
			bool ready 		= false;

			ESP_LOGD(TAG, "waiting for active interfaces...");

			do 
			{
extern int network_get_active_interfaces();				
				if ((ESP_OK == me_state.WIFI_init_res) || (ESP_OK == me_state.LAN_init_res))
				{
					int net_if_num = network_get_active_interfaces();

					if (net_if_num > 0)
					{
						ready = true;
					}
				}
				else
				{
					vTaskDelay(pdMS_TO_TICKS(200));
					timeout--;
				}
				
			} while (!ready && timeout);

			if (ready)
			{
				start_udp_receive_task(); 	// OK
				start_osc_recive_task(); 	// OK
				start_ftp_task(); 			// OK
				start_mdns_task();			// OK
				start_mqtt_task();			// OK
			}
			else
				sprintf(errorString, "Network initialization timeout (%d)", NETWORK_INIT_TIMEOUT);
		}
	}
	else
		ESP_LOGI(TAG, "Network disabled.");

	if (errorString[0])
	{
		ESP_LOGE(TAG, "%s", errorString);
		mblog(E, errorString);
	}

	return result;
}
void makeStatusReport(bool spread)
{
	char topic [ 64 ];
	char str [ 256 ];

	snprintf(str, sizeof(str) - 1, "Heap %d, %s, %s\n", 
			xPortGetFreeHeapSize(),
			networkGetStatusString(),
			usbGetStatusString()
			);

	if (spread)
	{
		usbprint(str);

		if (me_state.UDP_init_res == ESP_OK)
		{
			udplink_send(0, str);
		}

		if (me_state.MQTT_init_res == ESP_OK)
		{
			snprintf(topic, sizeof(topic) - 1, "%s/status", me_config.deviceName);
			mqtt_pub(topic, str);
		}
	}
	
	mblog(I, str);
}





void app_main(void)	
{

	setLogLevel(4);
	me_state.free_i2c_num=0;
	ESP_LOGD(TAG, "Start up");
	ESP_LOGD(TAG, "free Heap size %d", xPortGetFreeHeapSize());

	// for (int i = 0; i < 10; i++)
	// {
	// 	ESP_LOGD(TAG, "Wait a little...");
	// 	vTaskDelay(pdMS_TO_TICKS(1000));
	// }

	// initLeds();
	board_init(); // USB hardware
	
	mblog_init();

	nvs_init();

	xTaskCreatePinnedToCore(executer_task, "executer_task",  1024 * 4,NULL ,configMAX_PRIORITIES - 6, NULL, 0);
	
	
	me_state.sd_init_res = ESP_FAIL;
	me_state.sd_init_res = spisd_init();
	if (me_state.sd_init_res != ESP_OK)	{
		ESP_LOGE(TAG, "sdcard_init FAIL");
		const char *base_path = "/sdcard";
		const esp_vfs_fat_mount_config_t mount_config = {
				.max_files = 3,
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

	xTaskCreatePinnedToCore(usb_device_task, "usbd", USBD_STACK_SIZE, NULL, configMAX_PRIORITIES - 12, NULL,0);
	//xTaskCreateStatic(usb_device_task, "usbd", USBD_STACK_SIZE, NULL, configMAX_PRIORITIES - 20, usb_device_stack, &usb_device_taskdef);
	

	xTaskCreateStatic(cdc_task, "cdc", CDC_STACK_SZIE, NULL, configMAX_PRIORITIES - 2, cdc_stack, &cdc_taskdef);
	

	load_Default_Config();

//	scanFileSystem();
	sprintf(me_config.configFile, "/sdcard/%s", "config.ini");

	saveManifesto();

	initWorkPermissions();

	me_state.config_init_res = loadConfig();
	if (me_state.config_init_res != ESP_OK)	{
		char tmpString[40];
		sprintf(tmpString, "Load config FAIL in line: %d", me_state.config_init_res);
		mblog(E, tmpString);
	}

	mblog(I, "Log session begin");
	
	set_usb_debug();

	ESP_LOGD(TAG, "Free SPIRAM: %d bytes",heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

	me_state.slot_init_res = init_slots();

    fillSoundTrackList();

	//start_dwinUart_task(1);
	//debugTopicLists();
	
	if (strstr(me_config.slot_mode[0], "audioPlayer") != NULL) {
		me_state.content_search_res = loadContent();
		if (me_state.content_search_res != ESP_OK)	{
			ESP_LOGD(TAG, "Load Content FAIL");
			mblog(E, "Load content FAIL");
		}
	}else{
		me_state.content_search_res = ESP_FAIL;
	}

	start_scheduler_task();

	startNetworkServices();

	ESP_LOGI(TAG, "Ver %s. Load complite, start working. free Heap size %d", VERSION, xPortGetFreeHeapSize());
	//xTaskCreatePinnedToCore(heap_report, "heap_report",  1024 * 4,NULL ,configMAX_PRIORITIES - 16, NULL, 0);

	setWorkPermission(EVERY_SLOT);

	int freeHeapSize;
	int freeHeapSizeFLAG = 0;

	uint32_t periodicTicks = xTaskGetTickCount();
	uint32_t scheduleTicks = xTaskGetTickCount();

	while (1)
	{

		// if (xQueueReceive(exec_mailbox, &exec_message, (25 / portTICK_PERIOD_MS)) == pdPASS)
		// {
		// 	ESP_LOGD(TAG, "Exec mail incoming:%s", exec_message.str);
		// 	// char *event = exec_message.str + strlen(me_config.deviceName) + 1;
		// 	//execute(exec_message.str);
		// }
		// #define STATS_TICKS         pdMS_TO_TICKS(1000)
		
		// printf("\n\nGetting real time stats over %"PRIu32" ticks\n", STATS_TICKS);
        // esp_err_t ret = print_real_time_stats(STATS_TICKS);
		// if (ret == ESP_OK) {
        //     printf("Real time stats obtained\n");
        // } else {
        //     printf("Error getting real time stats: %s\n", esp_err_to_name(ret));
        // }
        // vTaskDelay(pdMS_TO_TICKS(1000));
		// char task_list[1024];
		// vTaskList(task_list);
		// ESP_LOGI(TAG, "Task list:\n%s", task_list);

		// UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        // ESP_LOGI(TAG, "Stack remaining: %u", stack_remaining);

		freeHeapSize = xPortGetFreeHeapSize();
		if (freeHeapSize < 4096)
		{
			if (!freeHeapSizeFLAG)
			{
				mblog(I, "Free heap size is LOW - %d bytes", freeHeapSize);
				freeHeapSizeFLAG = 1;
			}
		}
		else
			freeHeapSizeFLAG = 0;

		uint32_t now = xTaskGetTickCount();

		// every 15 minutes
		if (me_config.statusPeriod)
		{
			if ((now - periodicTicks) >= pdMS_TO_TICKS(me_config.statusPeriod * 1000)) 
			{
				makeStatusReport(me_config.statusAllChannels);

				periodicTicks = now;
			}
		}

		// every second 
		if ((now - scheduleTicks) >= pdMS_TO_TICKS(1000))
		{
			scheduler_periodic_turn();

			scheduleTicks = now;
		}
		
		vTaskDelay(pdMS_TO_TICKS(200));
	}
}

#define EVERYONE ((EventBits_t)(1 << 0))
#define THISONE  ((EventBits_t)(1 << (slot + 1)))

static StaticEventGroup_t uxWorkPermBuff;
static EventGroupHandle_t uxWorkPerm = 0;

void initWorkPermissions()
{
	uxWorkPerm = xEventGroupCreateStatic(&uxWorkPermBuff);
}
void setWorkPermission(int slot)
{
	if (slot == EVERY_SLOT)
	{
		ESP_LOGD(TAG, "Running all slots");
	}
	else	
		ESP_LOGD(TAG, "Running SLOT %d", slot);

	// EVERY_SLOT + 1 = 0
	xEventGroupSetBits(uxWorkPerm, 1 << (slot + 1));
}
void waitForWorkPermit_(int slot, const char * moduleName)
{
	EventBits_t bits;

	do 
	{
		bits = xEventGroupWaitBits(
			uxWorkPerm,
			EVERYONE | THISONE,
			pdFALSE,              	// Очистить ожидаемые биты при выходе
			pdFALSE,             	// Любой из указанных битов
			portMAX_DELAY);

	} while (!((bits & EVERYONE) || (bits & THISONE)));

	ESP_LOGD(TAG, "slot %d [%s] started working", slot, moduleName);
}
int workIsPermitted_(int slot, const char * moduleName)
{
	EventBits_t bits;

	bits = xEventGroupGetBits(uxWorkPerm);

	if (!((bits & EVERYONE) || (bits & THISONE)))
	{
		waitForWorkPermit_(slot, moduleName);
	}

	return 1;
}
#undef EVERYONE
#undef THISONE

uint32_t xQueueReceiveLast(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait)
{
	uint32_t result = pdFAIL;

	if (xQueueReceive(xQueue, pvBuffer, 0) == pdPASS)
	{
		// Очередь не пустая - выбираем всё до последнего без таймаута
		
		result = pdPASS;

		while (xQueueReceive(xQueue, pvBuffer, 0) == pdPASS)
		{
			// Nothing
		}
	}
	else
	{
		// Очередь пустая - ждём положенный таймаут
	
		result = xQueueReceive(xQueue, pvBuffer, xTicksToWait);
	}

	return result;
}

// USB Device Driver task
// This top level thread process all usb events and invoke callbacks
