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

#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "periph_touch.h"
#include "periph_button.h"
#include "input_key_service.h"
#include "periph_adc_button.h"
#include "board.h"

#include "hlk_sens.h"

#include <stdio.h>
#include <stdlib.h>

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
#include "in_out.h"


#include "myCDC.h"

#include "p9813.h"

#include "3n_mosfet.h"
#include "max7219_task.h"
#include "swiper.h"
#include "smartLed.h"
#include "someUnique.h"
#include "disp_hd44780.h"


extern uint8_t SLOTS_PIN_MAP[10][4];

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
extern void set_usb_debug(void);

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
	esp_log_level_set("MAX7219", level);
	esp_log_level_set("SWIPER", level);
	esp_log_level_set("SOME_UNIQUE", level);
	esp_log_level_set("DISP_HD44780", level);
	esp_log_level_set("HLK_SENS", level);
	}



void heap_report(){
	while(1){
		ESP_LOGD(TAG, "free Heap size %d", xPortGetFreeHeapSize());
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}

static esp_err_t print_real_time_stats(TickType_t xTicksToWait){
	#define configRUN_TIME_COUNTER_TYPE uint32_t
	#define ARRAY_SIZE_OFFSET 30
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    uint32_t total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
		printf("end_run_time:%ld - start_run_time:%ld\n",end_run_time, start_run_time);
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * portNUM_PROCESSORS);
            printf("| %s | %"PRIu32" | %"PRIu32"%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    //Common return path
    free(start_array);
    free(end_array);
    return ret;
}



void app_main(void)
{

	setLogLevel(4);

	ESP_LOGD(TAG, "Start up");
	ESP_LOGD(TAG, "free Heap size %d", xPortGetFreeHeapSize());

	// initLeds();
	board_init(); // USB hardware

	nvs_init();

	
	xTaskCreatePinnedToCore(executer_task, "executer_task",  1024 * 4,NULL ,configMAX_PRIORITIES - 12, NULL, 0);
	xTaskCreatePinnedToCore(usb_device_task, "usbd", USBD_STACK_SIZE, NULL, configMAX_PRIORITIES - 12, NULL,0);
	//xTaskCreateStatic(usb_device_task, "usbd", USBD_STACK_SIZE, NULL, configMAX_PRIORITIES - 20, usb_device_stack, &usb_device_taskdef);
	

	xTaskCreateStatic(cdc_task, "cdc", CDC_STACK_SZIE, NULL, configMAX_PRIORITIES - 2, cdc_stack, &cdc_taskdef);
	
	
	me_state.sd_init_res = ESP_FAIL;
	me_state.sd_init_res = spisd_init();
	if (me_state.sd_init_res != ESP_OK)	{
		ESP_LOGE(TAG, "sdcard_init FAIL");
		const char *base_path = "/sdcard";
		const esp_vfs_fat_mount_config_t mount_config = {
				.max_files = 2,
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
	
	set_usb_debug();

	me_state.slot_init_res = init_slots();
	//start_hlk2420_task(2);
	//start_buttonMatrix4_task(0);
	//start_disp_hd44780_task(2);
	//start_max7219_task(0);
	//start_swiper_task(0);
	//start_pn532Uart_task(0);
	
	//start_dwinUart_task(1);
	//debugTopicLists();
	
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
	//xTaskCreatePinnedToCore(heap_report, "heap_report",  1024 * 4,NULL ,configMAX_PRIORITIES - 16, NULL, 0);
	//testStepper();"startup"
	//crosslinks_process(me_config.startup_cross_link,"startup");
	//startup_crosslinks_exec();
	
	while (1)
	{

		// if (xQueueReceive(exec_mailbox, &exec_message, (25 / portTICK_PERIOD_MS)) == pdPASS)
		// {
		// 	ESP_LOGD(TAG, "Exec mail incoming:%s", exec_message.str);
		// 	// char *event = exec_message.str + strlen(me_config.device_name) + 1;
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

		vTaskDelay(pdMS_TO_TICKS(1000));

	}

	
}

// USB Device Driver task
// This top level thread process all usb events and invoke callbacks
