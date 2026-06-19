#include <stdio.h>
#include "myMqtt.h"
#include "stateConfig.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "executor.h"
#include "sdkconfig.h"

#include "esp_netif.h"
#include "mdns.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <mbdebug.h>



#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "mqtt";

extern stateStruct me_state;
extern configuration me_config;

char phonUp_State_topic[100];
char lifeTime_topic[100];

esp_mqtt_client_handle_t client;

char willTopic[255];

static esp_timer_handle_t mqtt_watchdog_timer = NULL;
static bool mqtt_watchdog_running = false;  // флаг: таймер уже тикает

/* Диагностика — обновляется из mqtt_event_handler, читается из reporter.
   Не используем мьютекс ради дешевизны — целые слова на ESP32 атомарны.
   Снимок берётся одним движением (struct copy). */
static mqtt_diag_t s_mqtt_diag = {0};

static void mqtt_watchdog_timer_cb(void *arg) {
    ESP_LOGE(TAG, "MQTT watchdog timeout! No connection for %d sec. Restarting...", me_config.mqttWatchdogTimeout);
    esp_restart();
}

static void mqtt_watchdog_start(void) {
    if (me_config.mqttWatchdogTimeout == 0) return;
    // Не перезапускаем таймер если уже тикает — иначе каждый неудачный
    // reconnect (DISCONNECTED каждые ~10с) будет сбрасывать watchdog
    if (mqtt_watchdog_running) return;
    if (mqtt_watchdog_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = mqtt_watchdog_timer_cb,
            .name = "mqtt_wd"
        };
        esp_timer_create(&timer_args, &mqtt_watchdog_timer);
    }
    esp_timer_start_once(mqtt_watchdog_timer, (uint64_t)me_config.mqttWatchdogTimeout * 1000000ULL);
    mqtt_watchdog_running = true;
    ESP_LOGD(TAG, "MQTT watchdog started: %d sec", me_config.mqttWatchdogTimeout);
}

static void mqtt_watchdog_stop(void) {
    if (mqtt_watchdog_timer != NULL && mqtt_watchdog_running) {
        esp_timer_stop(mqtt_watchdog_timer);
        mqtt_watchdog_running = false;
        ESP_LOGD(TAG, "MQTT watchdog stopped");
    }
}

/* ---- MQTT liveness watchdog (защита от half-open) --------------------------
   Событийный mqtt_watchdog взводится ТОЛЬКО по MQTT_EVENT_DISCONNECTED и
   бесполезен против half-open: TCP мёртв, но DISCONNECTED не приходит,
   is_connected остаётся 1 навсегда. Здесь — независимый механизм:
     - mqtt_heartbeat_task раз в MQTT_HB_PERIOD_S публикует QOS-1 heartbeat;
       при доставке брокер шлёт PUBACK -> MQTT_EVENT_PUBLISHED -> last_published_us.
     - отдельный esp_timer ТОЛЬКО читает s_mqtt_diag (без вызовов в mqtt-клиент,
       чтобы не упереться в тот же api_lock, если publish залип) и перезагружает
       плату, если подтверждённой активности не было дольше mqttWatchdogTimeout.
   Порог общий с обычным watchdog: mqttWatchdogTimeout==0 отключает оба. */
#define MQTT_HB_PERIOD_S 30

static esp_timer_handle_t mqtt_liveness_timer = NULL;

static void mqtt_heartbeat_task(void *arg) {
    char hbTopic[160];
    snprintf(hbTopic, sizeof(hbTopic), "clients/%s/hb", me_config.deviceName);
    char payload[24];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MQTT_HB_PERIOD_S * 1000));
        if (!s_mqtt_diag.is_connected) continue;
        uint64_t up_s = (uint64_t)(esp_timer_get_time() / 1000000);
        snprintf(payload, sizeof(payload), "%llu", (unsigned long long)up_s);
        /* QOS 1: при живом соединении прилетит PUBACK -> last_published_us
           обновится. При half-open publish не подтвердится (или зависнет),
           last_published_us устаревает -> сработает таймер ниже. */
        esp_mqtt_client_publish(client, hbTopic, payload, 0, 1, 0);
    }
}

static void mqtt_liveness_timer_cb(void *arg) {
    if (me_config.mqttWatchdogTimeout == 0) return;
    if (!s_mqtt_diag.is_connected) return;   /* disconnected -> ловит mqtt_watchdog */

    int64_t now = esp_timer_get_time();
    /* пока не было ни одного PUBLISHED — отсчёт от момента connect */
    int64_t ref = s_mqtt_diag.last_published_us ? s_mqtt_diag.last_published_us
                                                : s_mqtt_diag.last_connect_us;
    if (ref == 0) return;

    int64_t age_s = (now - ref) / 1000000;
    if (age_s > me_config.mqttWatchdogTimeout) {
        ESP_LOGE(TAG, "MQTT liveness timeout: %lld s without confirmed publish "
                      "(is_connected=1, half-open). Restarting...", (long long)age_s);
        mblog(E, "MQTT half-open frozen -> restart");
        esp_restart();
    }
}

static void mqtt_liveness_start(void) {
    if (me_config.mqttWatchdogTimeout == 0) return;
    if (mqtt_liveness_timer == NULL) {
        const esp_timer_create_args_t targs = {
            .callback = mqtt_liveness_timer_cb,
            .name = "mqtt_live"
        };
        esp_timer_create(&targs, &mqtt_liveness_timer);
        esp_timer_start_periodic(mqtt_liveness_timer, (uint64_t)MQTT_HB_PERIOD_S * 1000000ULL);
    }
    xTaskCreatePinnedToCore(mqtt_heartbeat_task, "mqtt_hb", 1024 * 3, NULL,
                            configMAX_PRIORITIES - 20, NULL, 0);
    ESP_LOGD(TAG, "MQTT liveness watchdog started: hb=%ds, timeout=%ds",
             MQTT_HB_PERIOD_S, me_config.mqttWatchdogTimeout);
}

extern QueueHandle_t exec_mailbox;

void mqtt_pub(const char *topic, const char *string){
    int msg_id = esp_mqtt_client_publish(client, topic, string, 0, me_config.mqttQOS, 0);
    //ESP_LOGD(TAG, "sent publish successful, msg_id=%d", msg_id);
}

void mqtt_diag_snapshot(mqtt_diag_t *out){
    if (!out) return;
    *out = s_mqtt_diag;
}

void mqtt_sub(const char *topic){
	esp_mqtt_client_subscribe(client, topic, me_config.mqttQOS);
	ESP_LOGD(TAG, "Subcribed successful, topic:%s", topic);
}

static int append_json_topic(char *dst, size_t dst_size, const char *topic, int *count)
{
	if (!dst || !topic || !count) {
		return -1;
	}

	int written = snprintf(dst + strlen(dst), dst_size - strlen(dst),
						   "%s\"%s\"", (*count > 0) ? ", " : "", topic);
	if (written < 0 || (size_t)written >= (dst_size - strlen(dst))) {
		return -1;
	}

	(*count)++;
	return 0;
}

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    //ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    //esp_mqtt_client_handle_t client = event->client;
    //int msg_id;
	

	switch ((esp_mqtt_event_id_t) event_id) {

	case MQTT_EVENT_CONNECTED:
		ESP_LOGD(TAG, "MQTT_EVENT_CONNECTED");
		me_state.MQTT_init_res = ESP_OK;
		s_mqtt_diag.connected++;
		s_mqtt_diag.last_connect_us = esp_timer_get_time();
		s_mqtt_diag.is_connected = 1;
		mqtt_watchdog_stop();

		for (int i = 0; i < NUM_OF_SLOTS; i++) {
			const char *action_topic = me_state.action_topic_list[i];
			if (action_topic && strncmp(action_topic, "none", 4) != 0) {
				size_t topic_len = strlen(action_topic);
				char *tmpS = malloc(topic_len + 3);
				if (tmpS == NULL) {
					ESP_LOGE(TAG, "OOM while building MQTT subscribe topic");
					continue;
				}
				snprintf(tmpS, topic_len + 3, "%s/#", action_topic);
				mqtt_sub(action_topic);
				mqtt_sub(tmpS);
				free(tmpS);
			}	
		}

		char tmpSB[255];
		snprintf(tmpSB, sizeof(tmpSB), "%s/system/#", me_config.deviceName);
		mqtt_sub(tmpSB);

		snprintf(willTopic, sizeof(willTopic), "clients/%s/state", me_config.deviceName);
		mqtt_pub(willTopic, "1");

		//----------topic list generate-----------------
		//---calc size
		// int topic_list_size = strlen("{\n\"triggers\":[\n")+strlen("\n],\n\"actions\":[\n")+strlen("\n]\n}");
		// for(int i=0; i<NUM_OF_SLOTS; i++){
		// 	if(memcmp(me_state.action_topic_list[i],"none", 4)!=0){
		// 		topic_list_size += strlen("\"\",\n");
		// 		topic_list_size+=strlen(me_state.action_topic_list[i]);
		// 	}
		// 	if(memcmp(me_state.trigger_topic_list[i],"none", 4)!=0){
		// 		topic_list_size += strlen("\"\",\n");
		// 		topic_list_size+=strlen(me_state.trigger_topic_list[i]);
		// 	}
		// }
		//---print to arrray
		char topic_list[1024] = { 0 };
		int count;

		snprintf(topic_list, sizeof(topic_list), "{ \"triggers\":[ ");
		count = 0;
		for (int i = 0; i < NUM_OF_SLOTS; i++) {
			const char *trigger_topic = me_state.trigger_topic_list[i];
			if (trigger_topic && strncmp(trigger_topic, "none", 4) != 0) {
				if (append_json_topic(topic_list, sizeof(topic_list), trigger_topic, &count) != 0) {
					ESP_LOGW(TAG, "Topic list truncated while appending trigger topics");
					break;
				}
			}
		}
		snprintf(topic_list + strlen(topic_list), sizeof(topic_list) - strlen(topic_list), " ], \"actions\":[ ");
		count = 0;
		for (int i = 0; i < NUM_OF_SLOTS; i++) {
			const char *action_topic = me_state.action_topic_list[i];
			if (action_topic && strncmp(action_topic, "none", 4) != 0) {
				if (append_json_topic(topic_list, sizeof(topic_list), action_topic, &count) != 0) {
					ESP_LOGW(TAG, "Topic list truncated while appending action topics");
					break;
				}
			}
		}
		snprintf(topic_list + strlen(topic_list), sizeof(topic_list) - strlen(topic_list), " ] }");

		char topicList_topic[255];
		snprintf(topicList_topic, sizeof(topicList_topic), "clients/%s/topics", me_config.deviceName);

		ESP_LOGD(TAG, "Topic list:%s", topic_list);
		mqtt_pub(topicList_topic, topic_list);
		//---declare action list to brocker

		break;
	case MQTT_EVENT_DISCONNECTED:
		ESP_LOGD(TAG, "MQTT_EVENT_DISCONNECTED");
		me_state.MQTT_init_res = ESP_FAIL;
		s_mqtt_diag.disconnected++;
		s_mqtt_diag.last_disconnect_us = esp_timer_get_time();
		s_mqtt_diag.is_connected = 0;
		mqtt_watchdog_start();
		break;

	case MQTT_EVENT_SUBSCRIBED:
		ESP_LOGD(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_UNSUBSCRIBED:
		ESP_LOGD(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_PUBLISHED:
		s_mqtt_diag.published++;
		s_mqtt_diag.last_published_us = esp_timer_get_time();
		ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_DATA:
		s_mqtt_diag.data++;
		s_mqtt_diag.last_data_us = esp_timer_get_time();
		//ESP_LOGI(TAG, "MQTT_EVENT_DATA");
		int max_needed = (event->topic_len > 0 ? event->topic_len : 0)
		               + (event->data_len > 0 ? event->data_len + 1 : 0) + 1;
		char *strT = malloc((size_t)max_needed);
		if (strT == NULL) {
			ESP_LOGE(TAG, "OOM while handling MQTT data event");
			break;
		}
		if(event->data_len > 0){
			snprintf(strT, (size_t)max_needed, "%.*s:%.*s", event->topic_len, event->topic, event->data_len, event->data);
		}else{
			snprintf(strT, (size_t)max_needed, "%.*s", event->topic_len, event->topic);
		}
		//sprintf(strT, "%.*s:%.*s", event->topic_len, event->topic, event->data_len, event->data);
		execute(strT);
		free(strT);

		break;
	case MQTT_EVENT_ERROR:
		s_mqtt_diag.errors++;
		ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
		if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
			log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
			log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
			log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
			ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

		}
		break;
	default:
		//ESP_LOGI(TAG, "Other event id:%d", event->event_id);
		break;
	}
}

int mqtt_app_start(void)
{
    uint32_t startTick = xTaskGetTickCount();
    uint32_t heapBefore = xPortGetFreeHeapSize();

    if(me_config.mqttBrokerAdress[0]==0){
        ESP_LOGW(TAG, "Empty mqtt broker adress, mqtt disable");
        return -1;
    }

	memset(willTopic, 0, sizeof(willTopic));
	snprintf(willTopic, sizeof(willTopic), "clients/%s/state", me_config.deviceName);

	char resolved_broker_host[64] = {0};
	snprintf(resolved_broker_host, sizeof(resolved_broker_host), "%s", me_config.mqttBrokerAdress);

	if(strstr(me_config.mqttBrokerAdress, ".local")!=NULL){
		size_t broker_len = strlen(me_config.mqttBrokerAdress);
		size_t host_len = broker_len;
		if (broker_len > 6) {
			host_len = broker_len - 6;  /* trim ".local" */
		}
		char hostname[64] = {0};
		if (host_len >= sizeof(hostname)) {
			host_len = sizeof(hostname) - 1;
		}
		memcpy(hostname, me_config.mqttBrokerAdress, host_len);
		hostname[host_len] = '\0';
		struct esp_ip4_addr addr;
    	addr.addr = 0;
    	ESP_LOGD(TAG, "Resolve hostname:%s", hostname);
		esp_err_t err = mdns_query_a(hostname, 2000,  &addr);
		if(err){
			if(err == ESP_ERR_NOT_FOUND){
				ESP_LOGW(TAG, "%s: Host was not found!", esp_err_to_name(err));
			}
			ESP_LOGE(TAG, "Query Failed: %s", esp_err_to_name(err));
		}else{
			ESP_LOGI(TAG, "Query A: %s.local resolved to: " IPSTR, hostname, IP2STR(&addr));
			snprintf(resolved_broker_host, sizeof(resolved_broker_host), IPSTR, IP2STR(&addr));
		}
	}

	// Build broker URI: mqtts:// for TLS, mqtt:// otherwise
	const char *scheme = me_config.mqttTLS ? "mqtts" : "mqtt";
	int default_port = me_config.mqttTLS ? 8883 : 1883;
	char brokerUri[96];
	snprintf(brokerUri, sizeof(brokerUri), "%s://%s:%d", scheme, resolved_broker_host, default_port);
	ESP_LOGD(TAG, "Set brokerUri:%s", brokerUri);
	
    esp_mqtt_client_config_t mqtt_cfg = {
    	.credentials.client_id = me_config.deviceName,
		.broker.address.uri = brokerUri,
		.session.keepalive = me_config.mqttKeepAlive,  // По умолчанию 60 — даёт запас при нагрузке на CPU
		.session.last_will.topic = willTopic,
		.session.last_will.msg = "0",
		// Таймаут сетевой операции: при half-open PINGREQ без PINGRESP в пределах
		// этого окна породит ошибку транспорта -> DISCONNECTED -> reconnect.
		.network.timeout_ms = 5000,
    };

	// Set login/password if configured
	if (me_config.mqttLogin[0] != 0) {
		mqtt_cfg.credentials.username = me_config.mqttLogin;
	}
	if (me_config.mqttPass[0] != 0) {
		mqtt_cfg.credentials.authentication.password = me_config.mqttPass;
	}

	// TLS: шифрование без проверки сертификата сервера (self-signed friendly)
	if (me_config.mqttTLS) {
		mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
		// Не используем crt_bundle_attach — позволяет подключаться к серверам
		// с самоподписанными сертификатами (шифрование без верификации CA).
		// Для production с проверкой CA: раскомментировать строку ниже.
		// mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
		ESP_LOGI(TAG, "MQTT TLS enabled (port %d, no CA verify)", default_port);
	}

	//ESP_LOGD(TAG, "Broker addr:%s", mqtt_cfg.uri);

    client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    //ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    //ESP_ERROR_CHECK(esp_mqtt_client_start(client));

	esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
	esp_mqtt_client_start(client);

	mqtt_watchdog_start();
	mqtt_liveness_start();

	ESP_LOGI(TAG, "MQTT QOS=%d, WatchdogTimeout=%d sec, KeepAlive=%d sec, retain=false, TLS=%s, auth=%s",
		me_config.mqttQOS, me_config.mqttWatchdogTimeout, me_config.mqttKeepAlive,
		me_config.mqttTLS ? "on" : "off",
		me_config.mqttLogin[0] ? "yes" : "no");
    ESP_LOGD(TAG, "MQTT init complite. Duration: %ld ms. Heap usage: %lu", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize());

    return ESP_OK;
}





