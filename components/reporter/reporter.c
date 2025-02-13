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
#include "esp_timer.h"
#include "myMqtt.h"

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "REPORTER";

#define MAILBOX_SIZE 10
#define MAX_STRING_LENGTH 512

int err;

typedef struct
{
	//char str[MAX_STRING_LENGTH];
	char *str;
	int  slot_num;
} reporter_message_t;

QueueHandle_t mailbox;

extern configuration me_config;
extern stateStruct me_state;

void crosslinker(char* str){
	uint64_t startTick = esp_timer_get_time();
	int slot_num;

	for(int i=0; i<NUM_OF_SLOTS; i++){
		if(strstr(str, me_state.trigger_topic_list[i])!=NULL){
			slot_num = i;
			break;
		}else{
			slot_num = -1;
		}
	}

	//ESP_LOGD(TAG, "Crosslinker incoming:%s slot_num:%d", str, slot_num);

	if ((slot_num >= 0)&&(strlen(me_config.slot_cross_link[slot_num])>0)){
		char crosslinks[strlen(me_config.slot_cross_link[slot_num])];
		strcpy(crosslinks, me_config.slot_cross_link[slot_num]);
		char *crosslink = NULL;
		char *croslink_rest = crosslinks;
		uint8_t last_link = 0;
		
		
		do{
			if (strstr(croslink_rest, ",") != NULL){
				crosslink = strtok_r(croslink_rest, ",", &croslink_rest);
				//to_do verify cross link len
			}else{
				crosslink = croslink_rest;
				last_link = 1;
			}
			//ESP_LOGD(TAG, "Cross_link:%s croslink_rest:%s lastLink flag:%d", crosslink, croslink_rest, last_link);
			if (strstr(crosslink, "->") != NULL){
				
				char eventMem[strlen(str)+1];
				strcpy(eventMem, str);
				char *event = eventMem;
				char *eventVal=NULL;
				if(strstr(event, me_config.deviceName)!=NULL){
					event = event + strlen(me_config.deviceName) + 1;
				}

				char *trigger=NULL;
				char *trigerVal=NULL;
				char *action=NULL;
				char *actionVal=NULL;

				char memForCopy[strlen(crosslink)+1];
				strcpy(memForCopy, crosslink);
				char *crosslinkCopy = memForCopy;

				if(strstr(crosslink, "->")!= NULL){
					action = strstr(crosslink, "->");
					*action = '\0';
					action+=2;
					if(strstr(action, ":")!= NULL){
						action = strtok_r(action, ":", &actionVal);
					}
					trigger = crosslink;
				}else{
					break;
				}
				//trigger = strtok_r(crosslinkCopy, "->", &action);
				//ESP_LOGD(TAG, "Trigger:%s action:%s actionVal:%s", trigger, action, actionVal);
				if(trigger[0]==' '){
					trigger = trigger+1;//  cut " " at begin
				}
				if(strstr(trigger, "#")!= NULL){
					if(strstr(trigger, ":")!= NULL){
						trigger = strtok(trigger, ":");
					}
					//ESP_LOGD(TAG, "Any value trigger:%s ", trigger);
				}else if(strstr(trigger, "@")!= NULL){
					if(strstr(trigger, ":")!= NULL){
						trigger = strtok_r(trigger, ":", &trigerVal);
						//ESP_LOGD(TAG, "Trigger:%s trigerVal:%s", trigger, trigerVal);
					}
					if(strstr(event, ":")!= NULL){
						event = strtok_r(event, ":", &eventVal);
						//ESP_LOGD(TAG, "Event:%s eventVal:%s", event, eventVal);
					}
					if((strstr(trigerVal, "==")!= NULL)||(strstr(trigerVal, ">")!= NULL)||(strstr(trigerVal, "<")!= NULL)){
						//ESP_LOGD(TAG, "Lets work whith bolean operator");
						char operator='0';
						char *leftVal=NULL;
						char *rightVal=NULL;
						int32_t val=0, threshold=0;
						if(strstr(trigerVal, "==")!= NULL){
							operator = '=';
							leftVal = strtok_r(trigerVal, "==",&rightVal);
							rightVal = rightVal + 1;// cut "==" at begin
						}else if(strstr(trigerVal, ">")!= NULL){
							operator = '>';
							leftVal = strtok_r(trigerVal, ">",&rightVal);
							rightVal = rightVal;// cut ">" at begin
						}else if(strstr(trigerVal, "<")!= NULL){
							operator = '<';
							leftVal = strtok_r(trigerVal, "<",&rightVal);
							rightVal = rightVal;// cut "<" at begin
						}

						//ESP_LOGD(TAG, "Bolean operator.  leftVal:%s  rightVal:%s", leftVal, rightVal);
						if(leftVal[0]=='@'){
							threshold = atoi(rightVal);
						}else{
							threshold = atoi(leftVal);
						}
						val = atoi(eventVal);

						if(operator == '='){
							if(val == threshold){
								val = 1;
							}else{
								val = 0;
							}
						}else if(operator == '>'){
							if(val > threshold){
								val = 1;
							}else{
								val = 0;
							}
						}else if(operator == '<'){
							if(val < threshold){
								val = 1;
							}else{
								val = 0;
							}
						}

						if(strchr(actionVal, '@')!= NULL){
							if(val == 1){
								actionVal="1\0";
							}else{
								actionVal="0\0";
							}
						}else{
							if(val != 1){
								//ESP_LOGD(TAG, "skip execution");
								goto skip;
							}
						}
						

						//ESP_LOGD(TAG, "Lets work whith bolean operator.  eventVal:%d  threshold:%d  operator:%c", val, threshold, operator);
					}else{
						actionVal = eventVal;
					}
					//ESP_LOGD(TAG, "Lets transfer event payload to action.  event:%s  payload:%s", event, payload);
				}
				//action = action + 1;// cut ":" at begin

				//ESP_LOGD(TAG, "Compare trigger:%s in event:%s", trigger, event);
				
				if (strstr(event, trigger) != NULL){
					//ESP_LOGD(TAG, "Crosslink event:%s, eventVal:%s trigger=%s, action=%s actionVal:%s", event, eventVal, trigger, action, actionVal);
					//ESP_LOGD(TAG, "strlen(me_config.deviceName):%d  strlen(action):%d", strlen(me_config.deviceName), strlen(action));
					
					char output_action[strlen(me_config.deviceName) + strlen(action) + 50];

					for(int i=0; i<NUM_OF_SLOTS; i++){
						//ESP_LOGD(TAG, "compare action:%s topic:%s",action, me_state.action_topic_list[i]);
						if(strstr(action, me_state.action_topic_list[i])!=NULL){
						    //ESP_LOGD(TAG, "Found custom action topic:%s", me_state.action_topic_list[i]);
							action = strtok(action, ":");
							strcpy(output_action, action);
							if(actionVal!=NULL){
								strcat(output_action, ":");
								strcat(output_action, actionVal);
							}
							goto exec;
						}
					}

					if(actionVal!=NULL){
						action = strtok(action, ":");
						sprintf(output_action, "%s/%s:%s", me_config.deviceName, action, actionVal);
					}else{
						sprintf(output_action, "%s/%s", me_config.deviceName, action);
					}
					
					
					exec:
					execute(output_action);
					//ESP_LOGD(TAG, "output_action:%s", output_action);
				}else{
					//ESP_LOGD(TAG, "BAD event:%s, trigger=%s, action=%s", event, trigger, action);
				}
				//vPortFree(crosslinkCopy);
			}
			
			skip:
			if (last_link == 1){
				//ESP_LOGD(TAG, "Break on last link");
				break;
			}
		} while (crosslink != NULL);
		

	}
	//ESP_LOGD(TAG, "Crosslink calc time:%lld", esp_timer_get_time() - startTick);
}

void send_report(char *tmpStr){
	usbprint(tmpStr);

	if(me_state.MQTT_init_res==ESP_OK){
		char tmpString[strlen(tmpStr)];
		strcpy(tmpString, tmpStr);
		char *payload;
		char *topic = strtok_r(tmpString, ":", &payload);
		mqtt_pub(topic, payload);
	}

	if(me_state.OSC_init_res==ESP_OK){
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
	if(me_state.UDP_init_res==ESP_OK){
		struct sockaddr_in destAddr = {0};
		destAddr.sin_addr.s_addr = inet_addr(me_config.udpServerAdress);
		destAddr.sin_family = 2;
		destAddr.sin_port = htons(me_config.udpServerPort);
		int res = sendto(me_state.udp_socket, tmpStr, strlen(tmpStr), 0, (struct sockaddr *)&destAddr, sizeof(destAddr));
		if (res < 0){
			ESP_LOGE(TAG,"Failed to send osc errno: %d string:%s\n", errno, tmpStr);
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
			
			send_report(tmpStr);
			
			crosslinker(tmpStr);
			heap_caps_free(received_message.str);
			//vPortFree(received_message.str);
		}	
	}
}

void reporter_init(void){
	me_state.reporter_queue=xQueueCreate(150, sizeof(reporter_message_t));
	xTaskCreatePinnedToCore(reporter_task, "reporter_task", 1024 * 4, NULL, configMAX_PRIORITIES - 20, NULL, 0);
	//xTaskCreate (reporter_task, "reporter_task", 1024 * 4, NULL, configMAX_PRIORITIES - 8, NULL);
}

void report(char *msg, int slot_num){
	//	memset(tmpStr, 0,64);
	//	sprintf(tmpStr, "monofonMSD\n");
	reporter_message_t send_message;

	//send_message.str = strdup(msg);

	char *copy = heap_caps_malloc(strlen(msg)+1, MALLOC_CAP_8BIT);
	strcpy(copy, msg);
	send_message.str = copy;

	send_message.slot_num = slot_num;
	esp_err_t ret = xQueueSend(me_state.reporter_queue, &send_message, 5);
	//ESP_LOGD(TAG, "Set message:%s to report queue: %d", send_message.str, send_message.slot_num);
	if(ret!= pdPASS){
		ESP_LOGE(TAG, "QueueSend error:%d", ret);
	}
	

	//free(send_message.str);
	//ESP_LOGD(TAG, "Set message:%s to report queue: %d", send_message.str, send_message.slot_num);

}

void reportState(){
	char tmpStr[512];  // Increased buffer size for JSON
    
	sprintf(tmpStr, "{");

    sprintf(tmpStr+ strlen(tmpStr), "\"freeRAM\":%d,", xPortGetFreeHeapSize());

    // Add WIFI network info if initialized successfully
    sprintf(tmpStr+ strlen(tmpStr), "\"WIFI_init_res\":%d," , me_state.WIFI_init_res);
	if (me_state.WIFI_init_res == ESP_OK) {
        sprintf(tmpStr + strlen(tmpStr), 
            "\"WIFI_SSID\":\"%s\",\"WIFI_ipAdress\":\"%s\",\"WIFI_netMask\":\"%s\",\"WIFI_gateWay\":\"%s\",",
			me_config.WIFI_ssid,
            me_config.WIFI_ipAdress,
            me_config.WIFI_netMask, 
            me_config.WIFI_gateWay
        );
    }

	sprintf(tmpStr+ strlen(tmpStr), "\"LAN_init_res\":%d," , me_state.LAN_init_res);
	if (me_state.LAN_init_res == ESP_OK) {
        sprintf(tmpStr + strlen(tmpStr), 
            "\"LAN_ipAdress\":\"%s\",\"LAN_netMask\":\"%s\",\"LAN_gateWay\":\"%s\",",
            me_config.LAN_ipAdress,
            me_config.LAN_netMask, 
            me_config.LAN_gateWay
        );
    }
    // Add other initialization states
    sprintf(tmpStr + strlen(tmpStr),
        "\"MQTT_init_res\":%d,"
        "\"UDP_init_res\":%d,"
        "\"OSC_init_res\":%d,"
        "\"FTP_init_res\":%d}",
        me_state.MQTT_init_res,
        me_state.UDP_init_res,
        me_state.OSC_init_res,
        me_state.FTP_init_res
    );

	send_report(tmpStr);
}

