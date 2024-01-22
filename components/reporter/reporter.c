#include <stdio.h>
#include <string.h>
#include "reporter.h"
#include "esp_log.h"
#include "myCDC.h"
#include "stateConfig.h"
#include "executor.h"
#include <tinyosc.h>
//#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
//#include <netdb.h>
#include <lwip/sockets.h>
//#include <netinet/in.h>

#include "myMqtt.h"

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "REPORTER";

#define MAILBOX_SIZE 10
#define MAX_STRING_LENGTH 255

int err;

typedef struct
{
	char str[MAX_STRING_LENGTH];
	int  slot_num;
} reporter_message_t;

QueueHandle_t mailbox;

extern configuration me_config;
extern stateStruct me_state;

void crosslinker(char* str)
{
	//ESP_LOGD(TAG, "Crosslinker incoming:%s", received_message.str);
	char *event = str + strlen(me_config.device_name) + 1;
	int slot_num;
	if (strstr(event, "player") != NULL)
	{
		slot_num = 0;
	}else{
		if (strstr(event, "_") != NULL){
			char *num_index = strstr(event, "_") + 1;
			slot_num = num_index[0] - '0';
		}else{
			slot_num = -1;
		}
	}
	//ESP_LOGD(TAG, "event:%s, slot_num=%d", event, slot_num);

	if ((slot_num >= 0)&&(strlen(me_config.slot_cross_link[slot_num])>0)){
		char crosslinks[strlen(me_config.slot_cross_link[slot_num])];
		strcpy(crosslinks, me_config.slot_cross_link[slot_num]);
		char *crosslink = NULL;
		char *croslink_rest = crosslinks;
		uint8_t last_link = 0;
		do{
			if (strstr(croslink_rest, ",") != NULL){
				crosslink = strtok_r(croslink_rest, ",", &croslink_rest);
				//TO_DO verify cross link len
			}
			else{
				crosslink = croslink_rest;
				last_link = 1;
			}
			if (strstr(crosslink, "->") != NULL){
				//ESP_LOGD(TAG, "Cross_link:%s", crosslink);
				char *trigger;
				char *action;

				trigger = strtok_r(crosslink, "->", &action);
				if(trigger[0]==' '){
					trigger = trigger+1;//  cut " " at begin
				}
				if(strstr(trigger, "*")!= NULL){
					trigger = strtok(trigger, ":");
					//ESP_LOGD(TAG, "Any value trigger:%s ", trigger);
				}
				action = action + 1;// cut ":" at begin

				//ESP_LOGD(TAG, "Compare trigger:%s in event:%s", trigger, event);
				if (strstr(event, trigger) != NULL){
					ESP_LOGD(TAG, "Crosslink event:%s, trigger=%s, action=%s", event, trigger, action);
					char output_action[strlen(me_config.device_name) + strlen(action) + 2];
					sprintf(output_action, "%s/%s", me_config.device_name, action);
					execute(output_action);
				}
				else{
					//ESP_LOGD(TAG, "BAD event:%s, trigger=%s, action=%s", event, trigger, action);
				}
			}
			if (last_link == 1){
				break;
			}

		} while (crosslink != NULL);
	}
}

void reporter_task(void){
	//char tmpStr[555];
	reporter_message_t received_message;
	for(;;){
		if (xQueueReceive(me_state.reporter_queue, &received_message, portMAX_DELAY) == pdPASS){
			//ESP_LOGD(TAG, "REPORT QueueReceive: %s len:%d from slot:%d", received_message.str, strlen(received_message.str), received_message.slot_num);
			int len = strlen(received_message.str)+strlen(me_state.trigger_topic_list[received_message.slot_num])+6;
			char tmpStr[len];
			memset(tmpStr, 0, strlen(tmpStr));

			//ESP_LOGD(TAG, "hueta tmpLen:%d topicLen:%d msgLen:%d",len,strlen(me_state.trigger_topic_list[received_message.slot_num]), strlen(received_message.str));
			if(received_message.str[0]=='/'){
				sprintf(tmpStr,"%s%s", me_state.trigger_topic_list[received_message.slot_num], received_message.str);
			}else{
				sprintf(tmpStr,"%s:%s", me_state.trigger_topic_list[received_message.slot_num], received_message.str);
			}
			//ESP_LOGD(TAG, "Report: %s", tmpStr);
			usbprint(tmpStr);

			if(me_state.LAN_init_res==ESP_OK){
				if(me_state.MQTT_init_res==ESP_OK){
					char tmpString[strlen(tmpStr)];
					strcpy(tmpString, tmpStr);
					char *payload;
					char *topic = strtok_r(tmpString, ":", &payload);
					mqtt_pub(topic, payload);
				}
				if((me_state.osc_socket >= 0)){
					char msg_copy[strlen(tmpStr)+1];
					if(tmpStr[0]!='/'){
						strcpy(msg_copy+1, tmpStr);
						msg_copy[0]='/';
					}else{
						strcpy(msg_copy, tmpStr);
					}
					char tmpString[strlen(msg_copy)+50];
					char *rest;
					char *tok = strtok_r(msg_copy, ":", &rest);
					
					int len=0;
					if(strstr(rest, ".")!=NULL){
						float tmp = atof(rest);
						len = tosc_writeMessage(tmpString, strlen(msg_copy)+20, tok, "f", tmp);
						ESP_LOGD(TAG, "OSC f string%s float:%f", tmpString, tmp );
					}else{
						int tmp = atoi(rest);
						len = tosc_writeMessage(tmpString, strlen(msg_copy)+20, tok, "i", tmp);
						ESP_LOGD(TAG, "OSC i string%s  tmp:%d", tmpString , tmp);
					}
					

					//len = tosc_writeMessage(tmpString, strlen(tmpString), tok, "s", rest);
					// Create an OSC message
					struct sockaddr_in destAddr = {0};
					destAddr.sin_addr.s_addr = inet_addr(me_config.oscServerAdress);
					destAddr.sin_family = 2;
					destAddr.sin_port = htons(me_config.oscServerPort);

					int res = sendto(me_state.osc_socket, tmpString, len, 0, (struct sockaddr *)&destAddr, sizeof(destAddr));
					if (res < 0){
						ESP_LOGE(TAG,"Failed to send osc errno: %d len:%d string:%s\n", errno, len, tmpString);
						err++;
						if(err>10){
							esp_restart();
						}
						return;
					}else{
						err--;
						if(err<0)err=0;
						//ESP_LOGD(TAG,"send osc OK: \n");
					}
				}
			}
			
			crosslinker(tmpStr);
			
		}	
	}
}

void reporter_init(void){
	me_state.reporter_queue=xQueueCreate(5, sizeof(reporter_message_t));
	xTaskCreatePinnedToCore(reporter_task, "reporter_task", 1024 * 4, NULL, configMAX_PRIORITIES - 8, NULL, 0);
	//xTaskCreate (reporter_task, "reporter_task", 1024 * 4, NULL, configMAX_PRIORITIES - 8, NULL);
}

void report(char *msg, int slot_num){
	//	memset(tmpStr, 0,64);
	//	sprintf(tmpStr, "monofonMSD\n");
	reporter_message_t send_message;
	strcpy(&send_message.str, msg);
	send_message.slot_num = slot_num;
	xQueueSend(me_state.reporter_queue, &send_message, portMAX_DELAY);
	//ESP_LOGD(TAG, "Set message to report queue: %d", send_message.slot_num);

}




void startup_crosslinks_exec(void){
	char crosslinks[strlen(me_config.startup_cross_link)];
	strcpy(crosslinks, me_config.startup_cross_link);
	char *crosslink = NULL;
	char *croslink_rest = crosslinks;
	uint8_t last_link = 0;
	do{
		if (strstr(croslink_rest, ",") != NULL){
			crosslink = strtok_r(croslink_rest, ",", &croslink_rest);
			//TO_DO verify cross link len
		}else{
			crosslink = croslink_rest;
			last_link = 1;
		}
		if (crosslink != NULL){
			//ESP_LOGD(TAG, "Cross_link:%s", crosslink);
			char *trigger;
			char *action;

			trigger = strtok_r(crosslink, "->", &action);
			action = action + 1;

			if (strstr(trigger, "startup") != NULL){
				ESP_LOGD(TAG, "Crosslink event:%s, trigger=%s, action=%s", "startup", trigger, action);
				char output_action[strlen(me_config.device_name) + strlen(action) + 2];
				sprintf(output_action, "%s/%s", me_config.device_name, action);
				execute(output_action);
			}
			else{
				// ESP_LOGD(TAG, "BAD event:%s, trigger=%s, action=%s", event, trigger, action);
			}
		}
		if (last_link == 1){
			break;
		}

	} while (crosslink != NULL);
}


