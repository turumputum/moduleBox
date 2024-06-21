#include <stdio.h>
#include "myMqtt.h"
#include "stateConfig.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "executor.h"
#include "sdkconfig.h"

#include "esp_netif.h"
#include "mdns.h"



#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "mqtt";

extern stateStruct me_state;
extern configuration me_config;

char phonUp_State_topic[100];
char lifeTime_topic[100];

esp_mqtt_client_handle_t client;

char willTopic[255];

extern QueueHandle_t exec_mailbox;

void mqtt_pub(const char *topic, const char *string){
    int msg_id = esp_mqtt_client_publish(client, topic, string, 0, 0, 1);
    //ESP_LOGD(TAG, "sent publish successful, msg_id=%d", msg_id);
}

void mqtt_sub(const char *topic){
	esp_mqtt_client_subscribe(client, topic, 0);
	ESP_LOGD(TAG, "Subcribed successful, topic:%s", topic);
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
		ESP_LOGD(TAG, "MQTT_CONNEKT_OK");

		for (int i = 0; i < NUM_OF_SLOTS; i++) {
			if(memcmp(me_state.action_topic_list[i],"none", 4)){
				char tmpS[strlen(me_state.action_topic_list[i])+3];
				sprintf(tmpS, "%s/#", me_state.action_topic_list[i]);
				mqtt_sub(me_state.action_topic_list[i]);
				mqtt_sub(tmpS);
			}	
		}

		sprintf(willTopic, "clients/%s/state", me_config.deviceName);
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
		char tmpStr[100];
		char topic_list[1024] = { 0 };
		strcat(topic_list,"{\n\"triggers\":[\n");
		for (int i = 0; i < NUM_OF_SLOTS; i++) {
			if(memcmp(me_state.trigger_topic_list[i],"none", 4)!=0){
				memset(tmpStr, 0, sizeof(tmpStr));
				sprintf(tmpStr, "\"%s\",\n", me_state.trigger_topic_list[i]);
				strcat(topic_list, tmpStr);
			}
		}
		topic_list[strlen(topic_list) - 2] = '\0';
		strcat(topic_list, "\n],\n\"actions\":[\n");
		for (int i = 0; i < NUM_OF_SLOTS; i++) {
			if(memcmp(me_state.action_topic_list[i],"none", 4)!=0){
				memset(tmpStr, 0, sizeof(tmpStr));
				sprintf(tmpStr, "\"%s\",\n", me_state.action_topic_list[i]);
				strcat(topic_list, tmpStr);
			}
		}
		topic_list[strlen(topic_list) - 2] = '\0';
		strcat(topic_list, "\n]\n}");

		char topicList_topic[255];
		sprintf(topicList_topic, "clients/%s/topics", me_config.deviceName);

		ESP_LOGD(TAG, "Topic list:%s", topic_list);
		mqtt_pub(topicList_topic, topic_list);
		//---declare action list to brocker

		break;
	case MQTT_EVENT_DISCONNECTED:
		ESP_LOGD(TAG, "MQTT_EVENT_DISCONNECTED");
		//esp_restart();
		break;

	case MQTT_EVENT_SUBSCRIBED:
		ESP_LOGD(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_UNSUBSCRIBED:
		ESP_LOGD(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_PUBLISHED:
		ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
		break;
	case MQTT_EVENT_DATA:
		ESP_LOGI(TAG, "MQTT_EVENT_DATA");
		char strT[255];
		sprintf(strT, "%.*s:%.*s", event->topic_len, event->topic, event->data_len, event->data);
		execute(strT);

		break;
	case MQTT_EVENT_ERROR:
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
	sprintf(willTopic, "clients/%s/state", me_config.deviceName);

	if(strstr(me_config.mqttBrokerAdress, ".local")!=NULL){
		char hostname[strlen(me_config.mqttBrokerAdress)-5];
		memcpy(hostname, me_config.mqttBrokerAdress, strlen(me_config.mqttBrokerAdress)-6);
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
			sprintf(me_config.mqttBrokerAdress, IPSTR, IP2STR(&addr));
		}
	}

	char brokerUri[strlen("mqtt://")+strlen(me_config.mqttBrokerAdress)+strlen(":1883")];
	sprintf(brokerUri, "mqtt://%s:1883", me_config.mqttBrokerAdress);
	ESP_LOGD(TAG, "Set brokerUri:%s", brokerUri);
	
    esp_mqtt_client_config_t mqtt_cfg = {
    	.credentials.client_id = me_config.deviceName,
		.broker.address.uri = brokerUri,
        //.broker.address.uri = "mqtt://192.168.88.99:1883"
		//.broker.address.hostname = me_config.mqttBrokerAdress,
		//TODO write will msg
		.session.keepalive = 15,
		.session.last_will.topic = willTopic,
		.session.last_will.msg = "0",
    };

	//ESP_LOGD(TAG, "Broker addr:%s", mqtt_cfg.uri);

    client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    //ESP_ERROR_CHECK(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    //ESP_ERROR_CHECK(esp_mqtt_client_start(client));

	esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
	esp_mqtt_client_start(client);

    ESP_LOGD(TAG, "MQTT init complite. Duration: %ld ms. Heap usage: %lu", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize());

    return ESP_OK;
}





