#include <stdio.h>
#include "WIFI.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_err.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "stateConfig.h"

#include "myCDC.h"
#include <mbdebug.h>

#define EXAMPLE_ESP_MAXIMUM_RETRY 50

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "WIFI";
static EventGroupHandle_t s_wifi_event_group;

extern uint8_t FLAG_PC_AVAILEBLE;
extern uint8_t FLAG_PC_EJECT;

extern stateStruct me_state;
extern configuration me_config;

static int s_retry_num = 0;

// static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
// 	if (event_id == WIFI_EVENT_AP_STACONNECTED) {
// 		wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t*) event_data;
// 		//ESP_LOGD(TAG, "Connectend client " MACSTR " join, AID=%d \r\n", MAC2STR(event->mac), event->aid);
// 	} else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
// 		wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t*) event_data;
// 		//ESP_LOGD(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac), event->aid);
// 		//sprintf(me_state.wifiApClientString, "\r\n");
// 	}
// }

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGD(TAG, "retry to connect to the AP");
		} else {
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGD(TAG, "connect to the AP fail");
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		sprintf(me_config.WIFI_ipAdress, IPSTR, IP2STR(&event->ip_info.ip));
		sprintf(me_config.WIFI_netMask, IPSTR, IP2STR(&event->ip_info.netmask));
		sprintf(me_config.WIFI_gateWay, IPSTR, IP2STR(&event->ip_info.gw));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		me_state.WIFI_init_res = ESP_OK;
	}
}

void wifi_scan(void) {
	char apList[128];
	ESP_ERROR_CHECK(esp_netif_init());
	//ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
	assert(sta_netif);

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	uint16_t number = 20;
	wifi_ap_record_t ap_info[number];
	uint16_t ap_count = 0;
	memset(ap_info, 0, sizeof(ap_info));

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	esp_wifi_scan_start(NULL, true);
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
	sprintf(apList,"Avalable networks = %u \r\n", ap_count);
	mblog(I, apList);
	printf(apList);
	for (int i = 0; (i < number) && (i < ap_count); i++) {
		sprintf(apList,"SSID \t%s", ap_info[i].ssid);
		mblog(I, apList);
			printf(apList);
		sprintf(apList,"\tRSSI \t%d", ap_info[i].rssi);
		mblog(I, apList);
			printf(apList);
		sprintf(apList,"\tChannel \t%d \n", ap_info[i].primary);
		mblog(I, apList);
			printf(apList);
	}


}

uint8_t wifiInit() {
	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();

	if(me_config.WIFI_enable==1){
		ESP_LOGI(TAG, "Start init wifi in station mode");

		if (me_config.WIFI_ssid[0] == 0) {
			ESP_LOGE(TAG, "WIFI ssid is empty");
			mblog(I, "WIFI ssid is empty");
			return ESP_FAIL;
		}

		if (me_config.WIFI_pass[0] == 0) {
			ESP_LOGE(TAG, "WIFI pass is empty");
			mblog(I, "WIFI pass is empty");
			return ESP_FAIL;
		}

		s_wifi_event_group = xEventGroupCreate();

		ESP_ERROR_CHECK(esp_netif_init());

		ESP_ERROR_CHECK(esp_event_loop_create_default());
		esp_netif_t *wifiSta = esp_netif_create_default_wifi_sta();

		if (me_config.WIFI_DHCP == 0) {
			ESP_ERROR_CHECK(esp_netif_dhcpc_stop(wifiSta));
			esp_netif_ip_info_t info_t;

			ip4addr_aton((const char*) me_config.WIFI_ipAdress, &info_t.ip);
			ip4addr_aton((const char*) me_config.WIFI_gateWay, &info_t.gw);
			ip4addr_aton((const char*) me_config.WIFI_netMask, &info_t.netmask);

			//
			// ESP_LOGD(TAG, "DHCP stoped");
			ESP_ERROR_CHECK(esp_netif_set_ip_info(wifiSta, &info_t));
			ESP_LOGD(TAG, "IP config complite");
			// esp_netif_dhcps_start(wifiSta);
			// ESP_LOGD(TAG, "DHCP started");
		}

		wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
		cfg.nvs_enable = false;
		ESP_ERROR_CHECK(esp_wifi_init(&cfg));

		ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
		ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

		wifi_config_t wifi_config = { .sta.ssid = "", .sta.password = "" };

		memcpy(wifi_config.sta.ssid, me_config.WIFI_ssid, sizeof(wifi_config.sta.ssid));
		memcpy(wifi_config.sta.password, me_config.WIFI_pass, sizeof(wifi_config.sta.ssid));
		// ESP_LOGD(TAG, "srt pass %s len %d", me_config.WIFI_pass, strlen(me_config.WIFI_pass));

		ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
		ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
		ESP_ERROR_CHECK(esp_wifi_start());

		/* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
			* number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
		EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
		WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
		pdFALSE,
		pdFALSE,
		portMAX_DELAY);

		/* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
		 * happened. */
		if (bits & WIFI_CONNECTED_BIT) {
			ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", wifi_config.sta.ssid, wifi_config.sta.password);
		} else if (bits & WIFI_FAIL_BIT) {
			ESP_LOGE(TAG, "Failed to connect to SSID:%s, password:%s", wifi_config.sta.ssid, wifi_config.sta.password);
			char tmpString[200];
			sprintf(tmpString, "Failed to connect to SSID:%s, password:%s", wifi_config.sta.ssid, wifi_config.sta.password);
			mblog(E, tmpString);
			esp_event_loop_delete_default();
			esp_netif_destroy_default_wifi(wifiSta);
			return ESP_FAIL;
		} else {
			ESP_LOGE(TAG, "UNEXPECTED EVENT");
		}
		ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
		ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
		vEventGroupDelete(s_wifi_event_group);
	} else {
		ESP_LOGD(TAG, "WIFI disable");
		return ESP_OK;
	}

	ESP_LOGD(TAG, "WIFI init complite. Duration: %ld ms. Heap usage: %lu free heap:%u", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize(),
			xPortGetFreeHeapSize());
	return ESP_OK;
}
