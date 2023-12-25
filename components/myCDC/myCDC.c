#include <stdio.h>

#include "tusb.h"
#include "stdbool.h"
#include "stateConfig.h"
#include "esp_wifi.h"

#include "esp_log.h"

#include "WIFI.h"
#include "executor.h"
#include "audioPlayer.h"

#define USB_PRINT_DELAY 10

uint8_t tmpbuf[128];

static int strBuffPtr = 0;
static char strBuff[128];

static char printfBuff[256];
extern configuration me_config;
extern stateStruct me_state;

extern uint8_t FLAG_PC_AVAILEBLE;
extern uint8_t FLAG_PC_EJECT;

//extern exec_message_t exec_message;
extern QueueHandle_t exec_mailbox;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "myCDC";

void usbprint(char *msg) {
	//printf("%s --- \r\n", msg);
	int cutLength = 63;
	char partBuff[cutLength + 1];
	int endPartLength = strlen(msg);
	char *endPart = msg;

	while (endPartLength > cutLength) {
		sprintf(partBuff, "%.*s", cutLength, endPart);
		//printf("%s --- \r\n", partBuff);
		endPart = endPart + cutLength;
		//printf("%s  \r\n", endPart);
		tud_cdc_write(partBuff, strlen(partBuff));
		tud_cdc_write_flush();
		vTaskDelay(pdMS_TO_TICKS(USB_PRINT_DELAY));
		endPartLength = strlen(endPart);
	}

	//printf("%d+++%s  \r\n",strlen(endPart), endPart);
	tud_cdc_write(endPart, strlen(endPart));
	tud_cdc_write_flush();
	vTaskDelay(pdMS_TO_TICKS(USB_PRINT_DELAY));
	tud_cdc_write("\n", 1);
	tud_cdc_write_flush();
	//printf("%s  \r\n", endPart);

}

void usbprintf(char *msg, ...) {
	va_list list;
	va_list args;
	int len;

	va_start(list, msg);
	va_copy(args, list);
	va_end(list);

	if ((len = vsprintf(printfBuff, msg, args)) > 0) {
		usbprint(printfBuff);
//		tud_cdc_write(printfBuff, len);
//		tud_cdc_write_flush();
	}
}

static void execCommand(char *cmd, int len) {
	if (!memcmp(cmd, "help", 4)) {
		usbprint("Monofon embedded\n"
				"List of avalible commands\n\n"
				"<Who are you?> - get device identifier and name\n\n"
				"<get wifi_status> - get wireless network status\n\n"
				"<get system_status> - get system status\n\n"
				"<set msc_enable 'val'> - enable/disable MSD mode, example: <set msc_enable 0>\n\n"
				"<set monofon_enable 'val'> - enable/disable device, example: <set enable 1>\n\n"
				"<reset config> set default configuration\n\n");

	} else if (!memcmp(cmd, "Who are you?", 12)) {
		ESP_LOGD(TAG, "usbReport who i am.");
		char tmpStr[64];
		sprintf(tmpStr, "moduleBox:%s\n", me_config.device_name);
		usbprint(tmpStr);
	} else if (!memcmp(cmd, "Get topic list.", 15)) {
		ESP_LOGD(TAG, "usbReport topiclist.");
		char tmpStr[255];
		for (int i = 0; i < NUM_OF_SLOTS; i++) {
			if(memcmp(me_state.trigger_topic_list[i],"none", 4)!=0){
				memset(tmpStr, 0, sizeof(tmpStr));
				sprintf(tmpStr, "triggers:%s\n", me_state.trigger_topic_list[i]);
				usbprint(tmpStr);
			}
		}
		for (int i = 0; i < NUM_OF_SLOTS; i++) {
			if(memcmp(me_state.action_topic_list[i],"none", 4)!=0){
				memset(tmpStr, 0, sizeof(tmpStr));
				sprintf(tmpStr, "actions:%s\n", me_state.action_topic_list[i]);
				usbprint(tmpStr);
			}
		}
		memset(tmpStr, 0, sizeof(tmpStr));
		len = sprintf(tmpStr, "End of topic list.\n");
		usbprint(tmpStr);
	} else if (!memcmp(cmd, "get system_status", 17)) {
		if (FLAG_PC_EJECT == 1) {
			char tmpStr[128];
			sprintf(tmpStr, "free Heap size %d \r\n", xPortGetFreeHeapSize());
			usbprint(tmpStr);
			if (me_state.sd_init_res != ESP_OK) {
				usbprint("SD_card error\r\n");
			}
			if (me_state.config_init_res != ESP_OK) {
				usbprint("Config error\r\n");
			}
			if (me_state.config_init_res != ESP_OK) {
				usbprint("Content error\r\n");
			}
			if (me_state.LAN_init_res != ESP_OK) {
				usbprint("wifi error\r\n");
			}
			if (me_state.MQTT_init_res != ESP_OK) {
				usbprint("mqtt error\r\n");
			}

			usbprint("Task Name\tStatus\tPrio\tHWM\tTask\tAffinity\n");
			char stats_buffer[1024];
			vTaskList(stats_buffer);
			//sprintf(tmpStr,"%s\n", stats_buffer);
			usbprint(stats_buffer);
			usbprint("\r\n");
		}
	} else if (!memcmp(cmd, "reset config", 12)) {
		if (FLAG_PC_EJECT == 1) {
			remove("/sdcard/config.ini");
			usbprint("OK\r\n Device will be rebooted\r\n");
			vTaskDelay(pdMS_TO_TICKS(USB_PRINT_DELAY));
			esp_restart();
		}
	}else if (len < 3) {
		// //printf("vot:%s\n", cmd);
		// ESP_LOGD(TAG, "unknown commnad: %s \n", cmd);
		// //usbprintf("unknown commnad: %s \n", cmd);
	}else{
		execute(cmd);
	}
}

//--------------------------------------------------------------------+
// USB CDC
//--------------------------------------------------------------------+
void cdc_task(void *params) {
	(void) params;
	ESP_LOGD(TAG, "CDC task started");
	// RTOS forever loop
	while (1) {
		// connected() check for DTR bit
		// Most but not all terminal client set this when making connection
		if (tud_cdc_connected()) {

			// connected and there are data available
			if (tud_cdc_available()) {
				char *on = (char*) &tmpbuf[0];

				uint32_t count = tud_cdc_read(tmpbuf, sizeof(tmpbuf));

//				tud_cdc_write(tmpbuf, count);
//				tud_cdc_write_flush();

				while (count--) {
					// dumb overrun protection
					if (strBuffPtr >= sizeof(strBuff)) {
						strBuffPtr = 0;
					}

					if (('\r' == *on) || ('\n' == *on)) {
						strBuff[strBuffPtr] = 0;

						execCommand(strBuff, strBuffPtr);

						strBuffPtr = 0;
					} else {
						strBuff[strBuffPtr] = *on;

						strBuffPtr++;
					}

					on++;
				}
			}
		}

		if(FLAG_PC_EJECT ==1){
      		esp_restart();
		}

		vTaskDelay(1);
	}
}

// Invoked when cdc when line state changed e.g connected/disconnected
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
	(void) itf;
	(void) rts;

	// TODO set some indicator
	if (dtr) {
		// Terminal connected
	} else {
		// Terminal disconnected
	}
}

// Invoked when CDC interface received data from host
void tud_cdc_rx_cb(uint8_t itf) {
	(void) itf;
}
