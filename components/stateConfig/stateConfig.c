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
#include <sys/stat.h>
#include "LAN.h"
#include "audio_error.h"
#include "audio_mem.h"
#include "me_slot_config.h"
#include <dirent.h>
//#include "buttonLed.h"
#include "help.h"
#include <axstring.h>
#include <mbdebug.h>
#include "esp_heap_caps.h"

#define TAG "stateConfig"

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#define SDCARD_SCAN_URL_MAX_LENGTH (255)

extern configuration me_config;
extern stateStruct me_state;

static int _yesno(const char * value)
{
	int result = 0;

	if ((*value >= '0') && (*value <= '9'))
	{
		// if just a fist char is digit
		result = atoi(value);
	}
	else if (!strcasecmp(value, "true") || !strcasecmp(value, "yes"))
	{
		result = 1;
	}

	return result;
}

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
		} else if (MATCH(str, "crosslink")) {
			pconfig->slot_crosslink[i] = strdup(value);
			return 1;
		}
	}

	if (MATCH("SYSTEM", "deviceName")) {//-----------------------------------------------
		pconfig->deviceName = strdup(value);
	} else if (MATCH("SYSTEM", "logLevel")) {//-----------------------------------------------

		//printf("@@@@@@@@@@@@ parsing loglevel stage 1\n");

		if (!strcasecmp(value, "none"))
			pconfig->logLevel	 = ESP_LOG_NONE;
		else if (!strcasecmp(value, "error"))
			pconfig->logLevel	 = ESP_LOG_ERROR;
		else if (!strcasecmp(value, "warn"))
			pconfig->logLevel	 = ESP_LOG_WARN;
		else if (!strcasecmp(value, "warning"))
			pconfig->logLevel	 = ESP_LOG_WARN;
		else if (!strcasecmp(value, "info"))
			pconfig->logLevel	 = ESP_LOG_INFO;
		else if (!strcasecmp(value, "debug"))
			pconfig->logLevel	 = ESP_LOG_DEBUG;
		else if (!strcasecmp(value, "verb"))
			pconfig->logLevel	 = ESP_LOG_VERBOSE;
		else if (!strcasecmp(value, "verbose"))
			pconfig->logLevel	 = ESP_LOG_VERBOSE;
		else
		{
			//printf("@@@@@@@@@@@@ parsing loglevel stage 2\n");

			pconfig->logLevel = atoi(value);

			if (!pconfig->logLevel)
			{
				ESP_LOGW(TAG, "Log level value '%s' not recugnized, use 'none' to disable log", value);

				pconfig->logLevel = ESP_LOG_NONE;
			}
			else if (pconfig->logLevel > ESP_LOG_VERBOSE)
			{
				ESP_LOGE(TAG, "logLevel value '%s' is too high, %d is max", value, (int)ESP_LOG_VERBOSE);
				pconfig->logLevel = ESP_LOG_VERBOSE;
			}
		}

	} else if (MATCH("SYSTEM", "logMaxSize")) {//-----------------------------------------------
		pconfig->logMaxSize = strz_to_bytes(value);
	} else if (MATCH("SYSTEM", "logChapters")) {//-----------------------------------------------
		pconfig->logChapters = atoi(value);
	} else if (MATCH("SYSTEM", "statusPeriod")) {//-----------------------------------------------
		pconfig->statusPeriod = atoi(value);
	} else if (MATCH("SYSTEM", "statusAllChannels")) {//-----------------------------------------------
		pconfig->statusAllChannels = _yesno(value);
	} else if (MATCH("SYSTEM", "boardVersion")) {//-----------------------------------------------
		pconfig->boardVersion = atoi(value);
	} else if (MATCH("SYSTEM", "USB_debug")) {
		pconfig->USB_debug = _yesno(value);
	}  else if (MATCH("LAN", "LAN_enable")) {//-----------------------------------------------
		pconfig->LAN_enable = _yesno(value);
	} else if (MATCH("LAN", "ipAdress")) {
		pconfig->LAN_ipAdress = strdup(value);
	} else if (MATCH("LAN", "netMask")) {
		pconfig->LAN_netMask = strdup(value);
	} else if (MATCH("LAN", "gateWay")) {
		pconfig->LAN_gateWay = strdup(value);
	} else if (MATCH("LAN", "DHCP")) {
		pconfig->LAN_DHCP = _yesno(value);
	} else if (MATCH("LAN", "speed")) {
		pconfig->LAN_speed = strdup(value);
	} else if (MATCH("WIFI", "WIFI_enable")) {//-----------------------------------------------
		pconfig->WIFI_enable = _yesno(value);
	} else if (MATCH("WIFI", "SSID")) {
		pconfig->WIFI_ssid = strdup(value);
	} else if (MATCH("WIFI", "pass")) {
		pconfig->WIFI_pass = strdup(value);
	} else if (MATCH("WIFI", "DHCP")) {
		pconfig->WIFI_DHCP = _yesno(value);
	} else if (MATCH("WIFI", "ipAdress")) {
		pconfig->WIFI_ipAdress = strdup(value);
	} else if (MATCH("WIFI", "netMask")) {
		pconfig->WIFI_netMask = strdup(value);
	} else if (MATCH("WIFI", "gateWay")) {
		pconfig->WIFI_gateWay = strdup(value);
	} else if (MATCH("WIFI", "channel")) {
		pconfig->WIFI_channel = atoi(value);
	} else if (MATCH("MDNS", "MDNS_enable")) {//-----------------------------------------------
		pconfig->MDNS_enable = _yesno(value);
	} else if (MATCH("FTP", "FTP_enable")) {//-----------------------------------------------
		pconfig->FTP_enable = atoi(value);
	} else if (MATCH("FTP", "FTP_anon")) {
		pconfig->FTP_anon = _yesno(value);
	} else if (MATCH("FTP", "FTP_login")) {
		pconfig->FTP_login = strdup(value);
	} else if (MATCH("FTP", "FTP_pass")) {
		pconfig->FTP_pass = strdup(value);
    } else if (MATCH("UDP", "serverAdress")){//-----------------------------------------------
        pconfig->udpServerAdress = strdup(value);
    } else if (MATCH("UDP", "serverPort")) {
        pconfig->udpServerPort = atoi(value);
    } else if (MATCH("UDP", "myPort")) {
        pconfig->udpMyPort = atoi(value);
	} else if (MATCH("UDP", "crosslink")) {
		pconfig->udp_crosslink = strdup(value);
	} else if (MATCH("OSC", "oscServerAdress")) {//-----------------------------------------------
		pconfig->oscServerAdress = strdup(value);
	} else if (MATCH("OSC", "oscServerPort")) {
		pconfig->oscServerPort = atoi(value);
	} else if (MATCH("OSC", "oscMyPort")) {
		pconfig->oscMyPort = atoi(value);
	} else if (MATCH("MQTT", "mqttBrokerAdress")) {//-----------------------------------------------
		pconfig->mqttBrokerAdress = strdup(value);
	} else if (MATCH("SCHEDULE", "ntpServer")) {//-----------------------------------------------
		pconfig->ntpServer = strdup(value);
	} else if (MATCH("SCHEDULE", "time")) {//-----------------------------------------------
		if (parse_schedule_time(value, &pconfig->scheduleEntries[pconfig->scheduleCount].time) == -1)
		{
			mblog(E, "Error parsing schedule time value: '%s'", value);
		}
	} else if (MATCH("SCHEDULE", "command")) {//-----------------------------------------------
		pconfig->scheduleEntries[pconfig->scheduleCount].command = strdup(value);
		pconfig->scheduleCount++;
	}else {
		return 0; /* unknown section/name, error */
	}
	return 1;
}

void load_Default_Config(void) {
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();

	me_config.deviceName = strdup("moduleBox");
	me_config.logLevel = ESP_LOG_WARN;
	me_config.logMaxSize = 50 * 1024;
	me_config.logChapters = 1;
	me_config.statusPeriod = 0;
	me_config.statusAllChannels = true;
	me_config.USB_debug = 0;
	me_config.boardVersion = 3;
	
	
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
	me_config.LAN_speed = strdup("auto");
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
    me_config.udp_crosslink = strdup("");
    me_state.UDP_init_res = ESP_FAIL;

	
	me_config.oscServerAdress = strdup("");
	me_config.oscServerPort = 0;
	me_config.oscMyPort = 0;
	me_state.OSC_init_res = ESP_FAIL;

	me_state.numOfTrack = 0;
	me_config.soundTracks = NULL;
	
	me_state.MQTT_init_res = ESP_FAIL;

	me_state.udplink_socket = -1;
	me_state.osc_socket = -1;

	me_state.eth_connected = -1;

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
		me_config.slot_crosslink[i] = str_2;
	}

	me_config.startup_crosslink = strdup("");

	ESP_LOGD(TAG, "Load default config complite. Duration:%ld ms. Heap usage:%lu free Heap:%u", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

uint8_t loadConfig(void) {
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();

	ESP_LOGD(TAG, "Init config");
	int res = ESP_OK;

	struct stat st;
	if (me_config.configFile[0] != 0 && stat(me_config.configFile, &st) == 0) {
		if (st.st_size < 102) {
			ESP_LOGW(TAG, "config file too small (%ld bytes), create default config", (long)st.st_size);
			saveConfig();
			return res;
		}
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
	sprintf(tmp, "speed = %s ;auto, 100M, 10M \r\n", me_config.LAN_speed);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));

    sprintf(tmp, "\r\n[UDP] \r\n");
    fprintf(configFile, tmp);
    memset(tmp, 0, strlen(tmp));
    sprintf(tmp, "server_adress = %s \r\n", me_config.udpServerAdress);
    fprintf(configFile, tmp);
    memset(tmp, 0, strlen(tmp));
    sprintf(tmp, "server_port = %d \r\n", me_config.udpServerPort);
    fprintf(configFile, tmp);
    memset(tmp, 0, strlen(tmp));
    sprintf(tmp, "my_port = %d \r\n", me_config.udpMyPort);
    fprintf(configFile, tmp);
    memset(tmp, 0, strlen(tmp));
	sprintf(tmp, "crosslink = %s \r\n", me_config.udp_crosslink);
	fprintf(configFile, tmp);
	memset(tmp, 0, strlen(tmp));


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
		sprintf(tmp, "crosslink = %s \r\n", "empty");
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
		ESP_LOGW(TAG, "Load content warning: No tracks found");
		return ESP_OK;
	}
}

uint8_t scan_dir(const char *path) {
	if (me_config.soundTracks == NULL) {
		ESP_LOGW(TAG, "scan_dir: soundTracks not allocated, use scanSoundTracks() instead");
		return 0;
	}
	FRESULT res;
	FF_DIR dir;
	FILINFO fno;
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
						char *detect = strrchr(fno.fname, '.');
						if (detect != NULL && strcasecmp(detect, ".mp3") == 0) {
							if (strcmp(path, "/") == 0) {
								sprintf(me_config.soundTracks[soundIndex], "/sdcard/%s", fno.fname);
							} else {
								sprintf(me_config.soundTracks[soundIndex], "/sdcard/%s/%s", path, fno.fname);
							}
							soundIndex++;
							}
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
	if (me_config.soundTracks == NULL) return;
	for (int i = 0; i < me_state.numOfTrack; i++) {
		ESP_LOGD(TAG, "{scanFileSystem} num:%d --- %s --- ", i, (char* )me_config.soundTracks[i]);
	}
}

uint8_t fillSoundTrackList() {
	ESP_LOGW(TAG, "fillSoundTrackList: deprecated, use scanSoundTracks() instead");
	return scanSoundTracks(".mp3");
}

//-----------------------------------------------------------------------
// scanSoundTracks — рекурсивное сканирование SD-карты, список в PSRAM
//-----------------------------------------------------------------------

static uint8_t _scanDir_psram(const char *fatfs_path, const char *vfs_prefix,
                               const char *extension, file_t *tracks, uint8_t count)
{
    FF_DIR dir;
    FILINFO fno;

    if (f_opendir(&dir, fatfs_path) != FR_OK) {
        ESP_LOGW(TAG, "scanSoundTracks: cannot open dir '%s'", fatfs_path);
        return count;
    }

    while (count < MAX_NUM_OF_TRACKS) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) {
            break;
        }
        // Пропускаем скрытые файлы/папки
        if (fno.fattrib & AM_HID) {
            continue;
        }

        // Промежуточные буферы с запасом для склейки путей
        char sub_fatfs[512];
        char sub_vfs[512];

        if (strcmp(fatfs_path, "/") == 0) {
            snprintf(sub_fatfs, sizeof(sub_fatfs), "/%s", fno.fname);
            snprintf(sub_vfs, sizeof(sub_vfs), "%s/%s", vfs_prefix, fno.fname);
        } else {
            snprintf(sub_fatfs, sizeof(sub_fatfs), "%s/%s", fatfs_path, fno.fname);
            snprintf(sub_vfs, sizeof(sub_vfs), "%s/%s", vfs_prefix, fno.fname);
        }

        if (fno.fattrib & AM_DIR) {
            // Рекурсия в подпапку
            count = _scanDir_psram(sub_fatfs, sub_vfs, extension, tracks, count);
        } else {
            // Проверяем расширение
            char *dot = strrchr(fno.fname, '.');
            if (dot != NULL && strcasecmp(dot, extension) == 0) {
                // Пропускаем файлы с путём длиннее лимита
                if (strlen(sub_vfs) >= FILE_NAME_LEGHT) {
                    ESP_LOGW(TAG, "scanSoundTracks: path too long, skipped: %s", sub_vfs);
                    continue;
                }
                strncpy(tracks[count], sub_vfs, FILE_NAME_LEGHT - 1);
                tracks[count][FILE_NAME_LEGHT - 1] = '\0';
                count++;
                //ESP_LOGD(TAG, "scanSoundTracks: [%d] %s", count - 1, tracks[count - 1]);
            }
        }
    }

    f_closedir(&dir);
    return count;
}

uint8_t scanSoundTracks(const char *extension)
{
    uint32_t startTick = xTaskGetTickCount();
    ESP_LOGI(TAG, "scanSoundTracks: searching for '%s' files...", extension);

    // Если массив уже выделен ранее — освобождаем
    if (me_config.soundTracks != NULL) {
        heap_caps_free(me_config.soundTracks);
        me_config.soundTracks = NULL;
    }
    me_state.numOfTrack = 0;

    // Выделяем массив MAX_NUM_OF_TRACKS записей в PSRAM
    size_t alloc_size = (size_t)MAX_NUM_OF_TRACKS * sizeof(file_t);
    me_config.soundTracks = (file_t *)heap_caps_calloc(MAX_NUM_OF_TRACKS, sizeof(file_t), MALLOC_CAP_SPIRAM);
    if (me_config.soundTracks == NULL) {
        ESP_LOGE(TAG, "scanSoundTracks: PSRAM alloc failed (%u bytes)", (unsigned)alloc_size);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "scanSoundTracks: allocated %u bytes in PSRAM", (unsigned)alloc_size);

    // Рекурсивное сканирование корня SD-карты
    uint8_t found = _scanDir_psram("/", "/sdcard", extension, me_config.soundTracks, 0);
    me_state.numOfTrack = found;

    // Сортировка по алфавиту
    if (found > 1) {
        qsort(me_config.soundTracks, found, sizeof(file_t),
              (int (*)(const void *, const void *))strcmp);
    }

    // Если треков мало — ужимаем блок в PSRAM до реального размера
    if (found == 0) {
        heap_caps_free(me_config.soundTracks);
        me_config.soundTracks = NULL;
        ESP_LOGW(TAG, "scanSoundTracks: no tracks found");
    } else if (found < MAX_NUM_OF_TRACKS) {
        file_t *shrunk = (file_t *)heap_caps_realloc(me_config.soundTracks,
                                                      (size_t)found * sizeof(file_t),
                                                      MALLOC_CAP_SPIRAM);
        if (shrunk != NULL) {
            me_config.soundTracks = shrunk;
        }
    }

    // // Логируем результат
    // for (int i = 0; i < found; i++) {
    //     ESP_LOGI(TAG, "  track[%d]: %s", i, me_config.soundTracks[i]);
    // }

    ESP_LOGI(TAG, "scanSoundTracks: found %d tracks in %ld ms",
             found, (xTaskGetTickCount() - startTick) * portTICK_RATE_MS);

    me_state.content_search_res = (found > 0) ? ESP_OK : ESP_FAIL;

    return (found > 0) ? ESP_OK : ESP_FAIL;
}

void debugTopicLists(void){
	for(int i=0; i<NUM_OF_SLOTS; i++){
		ESP_LOGD(TAG, "SLOT:%d Action topic:%s", i, me_state.action_topic_list[i]);
		ESP_LOGD(TAG, "SLOT:%d Trigger topic:%s", i, me_state.trigger_topic_list[i]);
	}
}

