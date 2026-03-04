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
#define WIFI_CONNECT_TIMEOUT_MS   (EXAMPLE_ESP_MAXIMUM_RETRY * 2000) // таймаут подключения
#define WIFI_RECONNECT_DELAY_MS   5000  // задержка между попытками переподключения

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define WIFI_SCAN_MAX_AP 10  // уменьшено с 20 для экономии стека

static const char *TAG = "WIFI";
static EventGroupHandle_t s_wifi_event_group;

extern uint8_t FLAG_PC_AVAILEBLE;
extern uint8_t FLAG_PC_EJECT;

extern stateStruct me_state;
extern configuration me_config;

static int s_retry_num = 0;
static bool wifi_was_connected = false;  // флаг: было ли успешное первое подключение

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		me_state.WIFI_init_res = ESP_FAIL;
		if (wifi_was_connected) {
			// После первого успешного подключения — всегда пытаемся переподключиться
			ESP_LOGW(TAG, "WiFi disconnected, reconnecting in %d ms...", WIFI_RECONNECT_DELAY_MS);
			vTaskDelay(pdMS_TO_TICKS(WIFI_RECONNECT_DELAY_MS));
			s_retry_num = 0;
			esp_wifi_connect();
		} else if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGD(TAG, "retry to connect to the AP (%d/%d)", s_retry_num, EXAMPLE_ESP_MAXIMUM_RETRY);
		} else {
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
			ESP_LOGD(TAG, "connect to the AP fail");
		}
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		snprintf(me_config.WIFI_ipAdress, 16, IPSTR, IP2STR(&event->ip_info.ip));
		snprintf(me_config.WIFI_netMask, 16, IPSTR, IP2STR(&event->ip_info.netmask));
		snprintf(me_config.WIFI_gateWay, 16, IPSTR, IP2STR(&event->ip_info.gw));
		s_retry_num = 0;
		wifi_was_connected = true;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
		me_state.WIFI_init_res = ESP_OK;
	}
}

void wifi_scan(void) {
	ESP_ERROR_CHECK(esp_netif_init());
	esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
	assert(sta_netif);

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	uint16_t number = WIFI_SCAN_MAX_AP;
	// Выделяем массив в куче вместо стека (~140 * 10 = 1400 байт)
	wifi_ap_record_t *ap_info = calloc(number, sizeof(wifi_ap_record_t));
	if (ap_info == NULL) {
		ESP_LOGE(TAG, "Failed to allocate memory for scan results");
		esp_wifi_deinit();
		esp_netif_destroy(sta_netif);
		return;
	}
	uint16_t ap_count = 0;

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_start());
	esp_wifi_scan_start(NULL, true);
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, ap_info));
	ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));

	ESP_LOGI(TAG, "Available networks = %u", ap_count);
	mblog(I, "Available networks = %u", ap_count);

	char apList[80];
	for (int i = 0; (i < number) && (i < ap_count); i++) {
		snprintf(apList, sizeof(apList), "SSID:%-32s RSSI:%d CH:%d",
				ap_info[i].ssid, ap_info[i].rssi, ap_info[i].primary);
		mblog(I, apList);
		ESP_LOGI(TAG, "%s", apList);
	}

	free(ap_info);

	// Освобождение ресурсов
	esp_wifi_stop();
	esp_wifi_deinit();
	esp_netif_destroy(sta_netif);
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

		s_retry_num = 0; // Сброс счётчика попыток

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

			ESP_ERROR_CHECK(esp_netif_set_ip_info(wifiSta, &info_t));
			ESP_LOGD(TAG, "IP config complite");
		}

		wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
		cfg.nvs_enable = false;
		ESP_ERROR_CHECK(esp_wifi_init(&cfg));

		ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
		ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

		wifi_config_t wifi_config;
		memset(&wifi_config, 0, sizeof(wifi_config));

		// Безопасное копирование SSID и пароля с правильными размерами
		strncpy((char*)wifi_config.sta.ssid, me_config.WIFI_ssid, sizeof(wifi_config.sta.ssid) - 1);
		strncpy((char*)wifi_config.sta.password, me_config.WIFI_pass, sizeof(wifi_config.sta.password) - 1);

		ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
		ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
		ESP_ERROR_CHECK(esp_wifi_start());

		/* Ожидание с конечным таймаутом вместо portMAX_DELAY */
		EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
		WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
		pdFALSE,
		pdFALSE,
		pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

		if (bits & WIFI_CONNECTED_BIT) {
			ESP_LOGI(TAG, "connected to ap SSID:%s", wifi_config.sta.ssid);
			// Обработчики остаются зарегистрированными для автоматического переподключения
		} else if (bits & WIFI_FAIL_BIT) {
			ESP_LOGE(TAG, "Failed to connect to SSID:%s", wifi_config.sta.ssid);
			mblog(E, "Failed to connect to SSID:%s", wifi_config.sta.ssid);
			// Полная очистка ресурсов при ошибке
			esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler);
			esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler);
			esp_wifi_stop();
			esp_wifi_deinit();
			esp_event_loop_delete_default();
			esp_netif_destroy_default_wifi(wifiSta);
			vEventGroupDelete(s_wifi_event_group);
			return ESP_FAIL;
		} else {
			// Таймаут — биты не выставлены
			ESP_LOGE(TAG, "WIFI connect timeout");
			mblog(E, "WIFI connect timeout");
			esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler);
			esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler);
			esp_wifi_stop();
			esp_wifi_deinit();
			esp_event_loop_delete_default();
			esp_netif_destroy_default_wifi(wifiSta);
			vEventGroupDelete(s_wifi_event_group);
			return ESP_FAIL;
		}
		// Event group больше не удаляем — он нужен для отслеживания переподключений
	} else {
		ESP_LOGD(TAG, "WIFI disable");
		return ESP_OK;
	}

	ESP_LOGD(TAG, "WIFI init complite. Duration: %ld ms. Heap usage: %lu free heap:%u", (xTaskGetTickCount() - startTick) * portTICK_PERIOD_MS, heapBefore - xPortGetFreeHeapSize(),
			xPortGetFreeHeapSize());
	return ESP_OK;
}
