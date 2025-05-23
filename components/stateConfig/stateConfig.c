#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stateConfig.h"
#include "ini.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "ff.h"
#include "diskio.h"
#include "sdcard_scan.h"
#include <errno.h>

#include "audio_error.h"
#include "audio_mem.h"
#include "me_slot_config.h"
#include <dirent.h>
//#include "buttonLed.h"
#include "help.h"

#define TAG "stateConfig"

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#define SDCARD_SCAN_URL_MAX_LENGTH (255)

extern configuration me_config;
extern stateStruct me_state;

static int handler(void *user, const char *section, const char *name, const char *value) {
	configuration *pconfig = (configuration*) user;

#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
	for(int i=0; i<NUM_OF_SLOTS; i++ ){
		//char str[10];
		//sprintf(str, "SLOT_%d", i);
		char str[]="SLOT_0";
		str[5]='0'+i;
		if (MATCH(str, "mode")) {
			pconfig->slot_mode[i] = strdup(value);
			return 1;
		} else if (MATCH(str, "options")) {
			pconfig->slot_options[i] = strdup(value);
			return 1;
		} else if (MATCH(str, "cross_link")) {
			pconfig->slot_cross_link[i] = strdup(value);
			return 1;
		}
	}

	if (MATCH("SYSTEM", "deviceName")) {//-----------------------------------------------
		pconfig->deviceName = strdup(value);
	} else if (MATCH("SYSTEM", "USB_debug")) {
		pconfig->USB_debug = atoi(value);
	}  else if (MATCH("LAN", "LAN_enable")) {//-----------------------------------------------
		pconfig->LAN_enable = atoi(value);
	} else if (MATCH("LAN", "ipAdress")) {
		pconfig->LAN_ipAdress = strdup(value);
	} else if (MATCH("LAN", "netMask")) {
		pconfig->LAN_netMask = strdup(value);
	} else if (MATCH("LAN", "gateWay")) {
		pconfig->LAN_gateWay = strdup(value);
	} else if (MATCH("LAN", "DHCP")) {
		pconfig->LAN_DHCP = atoi(value);
	} else if (MATCH("WIFI", "WIFI_enable")) {//-----------------------------------------------
		pconfig->WIFI_enable = atoi(value);
	} else if (MATCH("WIFI", "SSID")) {
		pconfig->WIFI_ssid = strdup(value);
	} else if (MATCH("WIFI", "pass")) {
		pconfig->WIFI_pass = strdup(value);
	} else if (MATCH("WIFI", "DHCP")) {
		pconfig->WIFI_DHCP = atoi(value);
	} else if (MATCH("WIFI", "ipAdress")) {
		pconfig->WIFI_ipAdress = strdup(value);
	} else if (MATCH("WIFI", "netMask")) {
		pconfig->WIFI_netMask = strdup(value);
	} else if (MATCH("WIFI", "gateWay")) {
		pconfig->WIFI_gateWay = strdup(value);
	} else if (MATCH("WIFI", "channel")) {
		pconfig->WIFI_channel = atoi(value);
	} else if (MATCH("MDNS", "MDNS_enable")) {//-----------------------------------------------
		pconfig->MDNS_enable = atoi(value);
	} else if (MATCH("FTP", "FTP_enable")) {//-----------------------------------------------
		pconfig->FTP_enable = atoi(value);
	} else if (MATCH("FTP", "FTP_anon")) {
		pconfig->FTP_anon = atoi(value);
	} else if (MATCH("FTP", "FTP_login")) {
		pconfig->FTP_login = strdup(value);
	} else if (MATCH("FTP", "FTP_pass")) {
		pconfig->FTP_pass = strdup(value);
	} else if (MATCH("UDP", "udpServerAdress")){//-----------------------------------------------
		pconfig->udpServerAdress = strdup(value);
	} else if (MATCH("UDP", "udpServerPort")) {
		pconfig->udpServerPort = atoi(value);
	} else if (MATCH("UDP", "udpMyPort")) {
		pconfig->udpMyPort = atoi(value);
	} else if (MATCH("UDP", "udp_cross_link")) {
		pconfig->udp_cross_link = strdup(value);
	} else if (MATCH("OSC", "oscServerAdress")) {//-----------------------------------------------
		pconfig->oscServerAdress = strdup(value);
	} else if (MATCH("OSC", "oscServerPort")) {
		pconfig->oscServerPort = atoi(value);
	} else if (MATCH("OSC", "oscMyPort")) {
		pconfig->oscMyPort = atoi(value);
	} else if (MATCH("MQTT", "mqttBrokerAdress")) {//-----------------------------------------------
		pconfig->mqttBrokerAdress = strdup(value);
	}else {
		return 0; /* unknown section/name, error */
	}
	return 1;
}

void writeErrorTxt(const char *buff) {

	FILE *errFile;

	errFile = fopen("/sdcard/error.txt", "a");
	if (!errFile) {
		ESP_LOGE(TAG, "fopen() failed");
		return;
	}
	unsigned int bytesWritten;
	bytesWritten = fprintf(errFile, buff);
	if (bytesWritten == 0) {
		ESP_LOGE(TAG, "fwrite() failed");
		return;
	}

	fclose(errFile);

}

void load_Default_Config(void) {
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();

	me_config.deviceName = strdup("moduleBox");
	me_config.USB_debug = 0;

	
	
	me_config.WIFI_enable = 0; // disable
	me_config.WIFI_DHCP = 1;
	me_config.WIFI_ipAdress = strdup("192.168.88.33");
	me_config.WIFI_netMask = strdup("255.255.255.0");
	me_config.WIFI_gateWay = strdup("192.168.88.1");
	me_config.WIFI_ssid = strdup("");
	me_config.WIFI_pass = strdup("");
	me_config.WIFI_channel = 6;
	me_state.WIFI_init_res = ESP_FAIL;

	me_config.LAN_enable = 0;
	me_config.LAN_DHCP = 1;
	me_config.LAN_ipAdress = strdup("192.168.88.33");
	me_config.LAN_netMask = strdup("255.255.255.0");
	me_config.LAN_gateWay = strdup("192.168.88.1");
	me_state.LAN_init_res = ESP_FAIL;

	me_config.MDNS_enable=1;

	me_config.FTP_enable = 1;
	me_config.FTP_anon = 1;
	me_config.FTP_login = strdup("user");
	me_config.FTP_pass = strdup("pass");
	me_state.FTP_init_res = ESP_FAIL;

	me_config.mqttBrokerAdress = strdup("");
	
	me_config.udpServerAdress = strdup("");
	me_config.udpServerPort = 0;
	me_config.udpMyPort = 0;
	me_config.udp_cross_link = strdup("");
	me_state.UDP_init_res = ESP_FAIL;
	
	me_config.oscServerAdress = strdup("");
	me_config.oscServerPort = 0;
	me_config.oscMyPort = 0;
	me_state.OSC_init_res = ESP_FAIL;

	me_state.numOfTrack = 0;
	
	me_state.MQTT_init_res = ESP_FAIL;

	me_state.udp_socket = -1;
	me_state.osc_socket = -1;


	me_state.slot_init_res = ESP_FAIL;
	
	me_state.ledc_chennelCounter = 0;

	for (int i = 0; i < NUM_OF_SLOTS; i++) {
		me_state.slot_task[i]=NULL;
		me_state.trigger_topic_list[i]=strdup("none");
		me_state.action_topic_list[i]=strdup("none");

		char *str_0 = calloc(1, sizeof(char));
		me_config.slot_mode[i] = str_0;
		char *str_1 = calloc(1, sizeof(char));
		me_config.slot_options[i] = str_1;
		char *str_2 = calloc(1, sizeof(char));
		me_config.slot_cross_link[i] = str_2;
	}

	me_config.startup_cross_link = strdup("");

	ESP_LOGD(TAG, "Load default config complite. Duration:%ld ms. Heap usage:%lu free Heap:%u", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

uint8_t loadConfig(void) {
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();

	ESP_LOGD(TAG, "Init config");
	int res = ESP_OK;

	if (me_config.configFile[0] != 0) {
		res = ini_parse(me_config.configFile, handler, &me_config);
		if (res != 0) {
			ESP_LOGE(TAG, "Can't load 'config.ini' check line: %d, set default\n", res);
			return res;
		}
	} else {
		ESP_LOGD(TAG, "config file not found, create default config");
		saveConfig();
		return res;
	}

	ESP_LOGD(TAG, "Load config complite. Duration:%ld ms. Heap usage:%ld free Heap:%u", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
	return res;

}

int saveConfig(void) {

	ESP_LOGD(TAG, "saving file");

	FILE *configFile;
	char tmp[200];

	if (remove("/sdcard/config.ini")) {
		//ESP_LOGD(TAG, "/sdcard/config.ini delete failed");
		//return ESP_FAIL;
	}

	configFile = fopen("/sdcard/config.ini", "w");
	if (!configFile) {
		ESP_LOGE(TAG, "fopen() failed");
		return ESP_FAIL;
	}

	sprintf(tmp, "  ;config file moduleBox. Ver:%s \r\n", VERSION);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));

	sprintf(tmp, "\r\n[SYSTEM] \r\n");
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));

	sprintf(tmp, "deviceName = %s \r\n", me_config.deviceName);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));

	sprintf(tmp, "\r\n[LAN] \r\n");
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "LAN_enable = %d ;0-disable, 1-enable \r\n", me_config.LAN_enable);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "DHCP = %d ;0-disable, 1-enable \r\n", me_config.LAN_DHCP);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "ipAdress = %s \r\n", me_config.LAN_ipAdress);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "netMask = %s \r\n", me_config.LAN_netMask);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "gateWay = %s \r\n", me_config.LAN_gateWay);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));

	sprintf(tmp, "\r\n[UDP] \r\n");
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "udpServerAdress = %s \r\n", me_config.udpServerAdress);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "udpServerPort = %d \r\n", me_config.udpServerPort);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "udpMyPort = %d \r\n", me_config.udpMyPort);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));

	
	// sprintf(tmp, "udp_cross_link = %s \r\n", me_config.udp_cross_link);
	// fprintf(configFile, tmp);
	// memset(tmp, 0, strlen(tmp));


	sprintf(tmp, "\r\n[OSC] \r\n");
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "oscServerAdress = %s \r\n", me_config.oscServerAdress);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "oscServerPort = %d \r\n", me_config.oscServerPort);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "oscMyPort = %d \r\n", me_config.oscMyPort);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));


	sprintf(tmp, "\r\n[MQTT] \r\n");
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "mqttBrokerAdress = %s \r\n", me_config.mqttBrokerAdress);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));

	for (int i = 0; i < 6; i++) {
		sprintf(tmp, "\r\n[SLOT_%d] \r\n", i);
		fprintf(configFile, tmp);
		memset(tmp, 0, strlen(tmp));

		sprintf(tmp, "mode = %s \r\n", "empty");
		fprintf(configFile, tmp);
		memset(tmp, 0, strlen(tmp));
		sprintf(tmp, "options = %s \r\n", "empty");
		fprintf(configFile, tmp);
		memset(tmp, 0, strlen(tmp));
		sprintf(tmp, "cross_link = %s \r\n", "empty");
		fprintf(configFile, tmp);
		memset(tmp, 0, strlen(tmp));
	}

	fprintf(configFile, help);

	vTaskDelay(pdMS_TO_TICKS(100));

	FRESULT res;
	res = fclose(configFile);
	if (res != ESP_OK) {
		ESP_LOGE(TAG, "fclose() failed: %d ", res);
		return ESP_FAIL;
	}

	ESP_LOGD(TAG, "save OK");
	return ESP_OK;
}

uint8_t loadContent(void) {
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();
	ESP_LOGD(TAG, "Loading content");

	if (me_state.numOfTrack > 0) {
		ESP_LOGI(TAG, "Load Content complete. numOfTrack:%d Duration: %ld ms. Heap usage: %lu", me_state.numOfTrack, (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize());
		return ESP_OK;
	} else {
		ESP_LOGE(TAG, "Load content fail");
		return ESP_FAIL;
	}
}

uint8_t scan_dir(const char *path) {
	FRESULT res;
	FF_DIR dir;
	FILINFO fno;
	uint8_t picIndex = 0;
	uint8_t soundIndex = 0;

	res = f_opendir(&dir, path);
	if (res == FR_OK) {
		ESP_LOGD(TAG, "{scanFileSystem} Open dir: %s", path);
		while (1) {
			res = f_readdir(&dir, &fno); /* Read a directory item */
			ESP_LOGD(TAG, "{scanFileSystem} File object:%s size:%lu dir:%d hid:%d", fno.fname, fno.fsize, (fno.fattrib & AM_DIR), (fno.fattrib & AM_HID));
			if ((res == FR_OK) && (fno.fname[0] != 0)) {
				if (!(fno.fattrib & AM_HID)) {
					if (fno.fattrib & AM_DIR) {
						scan_dir(fno.fname);
					} else {
						// file founded
						if (strcasecmp(fno.fname, "config.ini") == 0) {
							//ESP_LOGD(TAG, "{scanFileSystem} config file founded ");
							sprintf(me_config.configFile, "/sdcard/%s", fno.fname);
						} else if (strcasecmp(fno.fname, "intro.jpg") == 0) {
							//ESP_LOGD(TAG, "{scanFileSystem} introIco file founded ");
							sprintf(me_config.introIco, "/sdcard/%s", fno.fname);
						} else {
							char *detect = strrchr(fno.fname, '.');
							//ESP_LOGD(TAG, "{scanFileSystem} cut extension: %s ", detect);
							if (strcasecmp(detect, ".mp3") == 0) {
								//ESP_LOGD(TAG, "{scanFileSystem} soundFile founded ");
								if (strcmp(path, "/") == 0) {
									sprintf(me_config.soundTracks[soundIndex], "/sdcard/%s", fno.fname);
								} else {
									sprintf(me_config.soundTracks[soundIndex], "/sdcard/%s/%s", path, fno.fname);
								}
								soundIndex++;
							} else if (strcasecmp(detect, ".jpg") == 0) {
								//ESP_LOGD(TAG, "{scanFileSystem} iconFile founded ");
								if (strcmp(path, "/") == 0) {
									sprintf(me_config.trackIcons[picIndex], "/sdcard/%s", fno.fname);
								} else {
									sprintf(me_config.trackIcons[picIndex], "/sdcard/%s/%s", path, fno.fname);
								}
								picIndex++;
							}
						}
						//ESP_LOGD(TAG, "{scanFileSystem} File found ");
					}
				}
			} else {
				break;
			}
		}
	}
	return soundIndex;
}

void printTrackList() {
	for (int i = 0; i < MAX_NUM_OF_TRACKS; i++) {
		ESP_LOGD(TAG, "{scanFileSystem} num:%d --- %s --- %s", i, (char* )me_config.soundTracks[i], (char* )me_config.trackIcons[i]);
	}
}

uint8_t scanFileSystem() {
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();
	ESP_LOGD(TAG, "scanFileSystem");

	me_state.numOfTrack = 0;
	memset(me_config.configFile, 0, 254);
	memset(me_config.introIco, 0, 254);
	for (int i = 0; i < MAX_NUM_OF_TRACKS; i++) {
		memset(me_config.soundTracks[i], 0, 254);
		memset(me_config.trackIcons[i], 0, 254);
	}

	me_state.numOfTrack = scan_dir("/");
	ESP_LOGD(TAG, "scan end, lets sort");

	//---sorting sound trucks---
	if (me_state.numOfTrack > 1) {
		qsort(me_config.soundTracks, me_state.numOfTrack, FILE_NAME_LEGHT, (int (*)(const void*, const void*)) strcmp);
	}
	ESP_LOGD(TAG, "sort end, lets associate pic");
	//printTrackList();
	//---associate pic to track---
	for (int n = 0; n < me_state.numOfTrack; n++) {
		char *track_tmp;
		track_tmp = strdup(me_config.soundTracks[n]);
		if (strcmp(track_tmp, "") == 0) {
			char *name_sound = strtok(track_tmp, ".");
			if (strcmp(name_sound, "") == 0) {
				ESP_LOGD(TAG, "{scanFileSystem} search icon for: %s", name_sound);
				for (int i = 0; i < MAX_NUM_OF_TRACKS; i++) {

					if (strcmp(me_config.trackIcons[i], "") == 0) {
						char *icon_tmp;
						icon_tmp = strdup(me_config.trackIcons[i]);
						ESP_LOGD(TAG, "{scanFileSystem} Icon file: %s", me_config.trackIcons[i]);

						char *name_pic = strtok(icon_tmp, ".");
						if (strcmp(name_pic, "") == 0) {
							ESP_LOGD(TAG, "{scanFileSystem} validate: %s", name_pic);
							if (strcmp(name_sound, name_pic) == 0) {
								char s_tmp[FILE_NAME_LEGHT];
								sprintf(s_tmp, "%s", me_config.trackIcons[n]);
								memset(me_config.trackIcons[n], 0, 254);
								//sprintf(me_config.trackIcons[n], "%s", me_config.trackIcons[i]);
								strcpy(me_config.trackIcons[n], me_config.trackIcons[i]);
								memset(me_config.trackIcons[i], 0, 254);
								sprintf(me_config.trackIcons[i], "%s", s_tmp);
								//ESP_LOGD(TAG,"{scanFileSystem} name is correct");
							}
						}
					}
				}
				//--- remove unused images from list ---
				ESP_LOGD(TAG, "{scanFileSystem} remove unused pic ");
				if (strcmp(me_config.trackIcons[n], "") == 0) {
					char *icon_tmp;
					icon_tmp = strdup(me_config.trackIcons[n]);
					char *name_pic = strtok(icon_tmp, ".");
					if (strcmp(name_sound, name_pic) != 0) {
						memset(me_config.trackIcons[n], 0, 254);
					}
				}
				ESP_LOGD(TAG, "{scanFileSystem} unused pic deleted from list");
			}
		}
	}
	printTrackList();

	ESP_LOGI(TAG, "Filesystem scan complete. numOfTrack:%d Duration: %ld ms. Heap usage: %lu", me_state.numOfTrack, (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize());
	return ESP_OK;

}

void debugTopicLists(void){
	for(int i=0; i<NUM_OF_SLOTS; i++){
		ESP_LOGD(TAG, "SLOT:%d Action topic:%s", i, me_state.action_topic_list[i]);
		ESP_LOGD(TAG, "SLOT:%d Trigger topic:%s", i, me_state.trigger_topic_list[i]);
	}
}

