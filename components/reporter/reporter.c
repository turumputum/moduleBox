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
#include "LAN.h"
#include "udplink.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include <fcntl.h>
#include <stdbool.h>
#include <mbdebug.h>

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "REPORTER";

#define MAILBOX_SIZE 10
#define MAX_STRING_LENGTH 512

typedef struct
{
	//char str[MAX_STRING_LENGTH];
	char *str;
	int  slot_num;
} reporter_message_t;

/* --- Транспорты доставки отчётов ---------------------------------------
   У каждого транспорта СВОЙ буфер и СВОЯ задача. Раньше была одна общая
   очередь с одним глобальным гейтом по линку: лежащий транспорт держал отчёты
   для всех, а ошибка отправки в один (return в send_report) глотала отправку
   в остальные.
   Очередь заводится ТОЛЬКО под реально настроенный транспорт - не настроен,
   значит нет очереди, нет копий строк и нет памяти под них.

   USB CDC - такой же транспорт со своим буфером, а не "живая консоль". Раньше
   forward_report звал usbprint синхронно, а тот молча выходит, пока
   tud_is_plugged() == 0. Энумерация хоста занимает сотни мс, стартовые же
   отчёты уходят через ~30 мс после подъёма USB-стека - и терялись все до
   единого. Теперь они копятся кольцом и выливаются в консоль, как только хост
   перечислил устройство. */
#define TR_QUEUE_DEPTH  100

typedef enum { TR_USB = 0, TR_MQTT, TR_OSC, TR_UDP, TR_COUNT } tr_id_t;

typedef struct
{
	const char *    name;
	QueueHandle_t   q;          // NULL - транспорт не настроен, отчёты игнорируем
	uint32_t        dropped;    // выкинуто при переполнении своего буфера
	bool            buffering;
} transport_t;

static transport_t s_tr[TR_COUNT] =
{
	[TR_USB]  = { .name = "usb"  },
	[TR_MQTT] = { .name = "mqtt" },
	[TR_OSC]  = { .name = "osc"  },
	[TR_UDP]  = { .name = "udp"  },
};

QueueHandle_t mailbox;

extern configuration me_config;
extern stateStruct me_state;

void forward_report(char *msg, int slot_num);

// Ожидается ли сеть вообще. Если ни один интерфейс не включён -
// буферизировать отчёты незачем, просто выгребаем и отбрасываем.
static inline bool reporter_net_expected(void)
{
	return me_config.LAN_enable || me_config.WIFI_enable;
}

/* Настроен ли транспорт - решается по КОНФИГУ, один раз при reporter_init.
   Предикаты те же, по которым транспорт вообще поднимается: MQTT - LAN.c,
   OSC-UDP - адрес сервера (без него отправлять некуда). */
static bool tr_configured(tr_id_t id)
{
	/* USB CDC есть на плате всегда и от сети не зависит - буфер заводим
	   безусловно, чтобы стартовые отчёты дождались подключения консоли. */
	if (id == TR_USB) return true;

	if (!reporter_net_expected()) return false;

	switch (id)
	{
		case TR_MQTT: return me_config.mqttBrokerAdress && (strlen(me_config.mqttBrokerAdress) > 3);
		case TR_OSC:  return me_config.oscServerAdress  && (strlen(me_config.oscServerAdress)  > 3);
		case TR_UDP:  return me_config.udpServerAdress  && (strlen(me_config.udpServerAdress)  > 3);
		default:      return false;
	}
}

void crosslinker_(char* 	str,
				  char * 	rules)
{
	char crosslinks[strlen(rules) + 1];
	strcpy(crosslinks,  rules);
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
			/* После strtok_r по ',' второй и далее кросслинки могут начинаться
			   с одного или нескольких пробелов/табов (пользователи часто
			   пишут `, ...` или `,  ...` для читаемости). Срезаем все. */
			while(trigger[0]==' ' || trigger[0]=='\t'){
				trigger++;
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
				if(trigerVal!=NULL && ((strstr(trigerVal, "==")!= NULL)||(strstr(trigerVal, ">")!= NULL)||(strstr(trigerVal, "<")!= NULL))){
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
					if(me_state.action_topic_list[i]==NULL) continue;
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

				/* strtok может вернуть NULL для пустой/только-разделитель строки.
				   Также действие могло потеряться из-за повреждения буфера. Защищаемся. */
				action = strtok(action, ":");
				if(action==NULL){
					ESP_LOGW(TAG, "crosslink: action NULL after strtok, skipping");
					goto skip;
				}
				if(actionVal!=NULL){
					sprintf(output_action, "%s/%s:%s", me_config.deviceName, action, actionVal);
				}else{
					sprintf(output_action, "%s/%s", me_config.deviceName, action);
				}
				
				
				exec:
				/* CrossLink debug: публикуем исполняемую строку линкера в
				   <deviceName>/crossLink/execute - удобно смотреть работу кросслинкера */
				if(me_config.crossLink_debug){
					char dbg[strlen(me_config.deviceName) + strlen(output_action) + 24];
					sprintf(dbg, "%s/crossLink/execute:%s", me_config.deviceName, output_action);
					forward_report(dbg, -1);
				}
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

void crosslinker(char* str){
	uint64_t startTick = esp_timer_get_time();
	int slot_num;

	for(int i=0; i<NUM_OF_SLOTS; i++){
		if(me_state.trigger_topic_list[i]==NULL){
			slot_num = -1;
			continue;
		}
		if(strstr(str, me_state.trigger_topic_list[i])!=NULL){
			slot_num = i;
			break;
		}else{
			slot_num = -1;
		}
	}

	//ESP_LOGD(TAG, "Crosslinker incoming:%s slot_num:%d", str, slot_num);

	if ((slot_num >= 0)&&(strlen(me_config.slot_crosslink[slot_num])>0))
	{
		crosslinker_(str, me_config.slot_crosslink[slot_num]);
	}
	//ESP_LOGD(TAG, "Crosslink calc time:%lld", esp_timer_get_time() - startTick);
}
/* Отправка в конкретный транспорт. Раньше это был один send_report со всеми
   тремя ветками подряд, и ошибка отправки в OSC делала return - выход из всей
   функции, то есть глотала отправку в UDP. Теперь транспорты независимы:
   у каждого своя функция, свой буфер и своя задача. */
static void tr_send_mqtt(const char * tmpStr)
{
	char tmpString[strlen(tmpStr) + 1];
	strcpy(tmpString, tmpStr);
	char *payload;
	char *topic = strtok_r(tmpString, ":", &payload);

	mqtt_pub(topic, payload);
}

static void tr_send_udp(int slot_num, const char * tmpStr)
{
	int res = udplink_send(slot_num, tmpStr);

	if (res < 0){
		ESP_LOGW(TAG,"Failed to send UDP errno: %d string:%s", errno, tmpStr);
	}
}

static void tr_send_osc(const char * tmpStr)
{
	{
		char msg_copy[strlen(tmpStr) + 2];
		if(tmpStr[0] != '/'){
			msg_copy[0] = '/';
			strcpy(msg_copy + 1, tmpStr);
		}else{
			strcpy(msg_copy, tmpStr);
		}

		char tmpString[strlen(msg_copy)+50];
		char *rest;
		char *tok = strtok_r(msg_copy, ":", &rest);
		
		/* Автоопределение типа OSC-значения по payload:
		   целое -> 'i', дробное -> 'f', всё остальное (строки, напр buttonMatrix) -> 's'.
		   Пустой payload остаётся 'i' (atoi -> 0) ради совместимости. */
		char osc_type = 'i';
		if(rest != NULL && rest[0] != 0){
			const char *p = rest;
			int digits = 0, dots = 0, other = 0;
			if(*p == '+' || *p == '-') p++;
			for(; *p; p++){
				if(*p >= '0' && *p <= '9') digits++;
				else if(*p == '.') dots++;
				else other++;
			}
			if(other == 0 && digits > 0 && dots <= 1){
				osc_type = (dots == 1) ? 'f' : 'i';
			}else{
				osc_type = 's';
			}
		}

		int len=0;
		if(osc_type == 'f'){
			float tmp = atof(rest);
			len = tosc_writeMessage(tmpString, strlen(msg_copy)+20, tok, "f", tmp);
			ESP_LOGD(TAG, "OSC f %s float:%f", tmpString, tmp);
		}else if(osc_type == 's'){
			len = tosc_writeMessage(tmpString, strlen(msg_copy)+20, tok, "s", rest);
			ESP_LOGD(TAG, "OSC s %s str:%s", tmpString, rest);
		}else{
			int tmp = atoi(rest);
			len = tosc_writeMessage(tmpString, strlen(msg_copy)+20, tok, "i", tmp);
			ESP_LOGD(TAG, "OSC i %s tmp:%d", tmpString, tmp);
		}
		

		//len = tosc_writeMessage(tmpString, strlen(tmpString), tok, "s", rest);
		// Create an OSC message
		struct sockaddr_in destAddr = {0};
		destAddr.sin_addr.s_addr = inet_addr(me_config.oscServerAdress);
		destAddr.sin_family = 2;
		destAddr.sin_port = htons(me_config.oscServerPort);

		int res = sendto(me_state.osc_socket, tmpString, len, 0, (struct sockaddr *)&destAddr, sizeof(destAddr));
		if (res < 0){
			ESP_LOGW(TAG,"Failed to send osc errno: %d len:%d string:%s", errno, len, tmpString);
			// сеть могла отвалиться - не перезагружаемся: отчёты снова уйдут в
			// собственный буфер OSC, как только его init_res сбросится
		}
	}
}

// Готов ли транспорт принимать отправку прямо сейчас (рантайм).
static bool tr_ready(tr_id_t id)
{
	switch (id)
	{
		/* Хост перечислил устройство. Ровно это же проверяет usbprint внутри,
		   но здесь проверка ДО отправки - отчёт ждёт в буфере, а не пропадает. */
		case TR_USB:  return usb_console_ready();
		case TR_MQTT: return me_state.MQTT_init_res == ESP_OK;
		case TR_OSC:  return me_state.OSC_init_res  == ESP_OK;
		case TR_UDP:  return me_state.UDP_init_res  == ESP_OK;
		default:      return false;
	}
}

/* Задача одного транспорта. Копит свою очередь, пока ЕГО транспорт не готов,
   и выгребает её, как только тот поднялся. Гейт персональный: лежащий MQTT
   больше не задерживает OSC-UDP и наоборот. */
static void transport_task(void *arg)
{
	tr_id_t         id  = (tr_id_t)(intptr_t)arg;
	transport_t *   t   = &s_tr[id];
	reporter_message_t m;

	for(;;)
	{
		if (!tr_ready(id))
		{
			if (!t->buffering)
			{
				t->buffering = true;
				// Только в UART, НЕ в mblog: на старте транспорт ещё не поднят,
				// это срабатывает при каждой загрузке - незачем писать на флешку
				// и мутировать FAT.
				ESP_LOGW(TAG, "reporter[%s] - down, buffering reports", t->name);
			}
			vTaskDelay(pdMS_TO_TICKS(200));
			continue;
		}

		if (t->buffering)
		{
			t->buffering = false;
			ESP_LOGI(TAG, "reporter[%s] - up, flushing %d buffered reports",
					t->name, (int)uxQueueMessagesWaiting(t->q));
			t->dropped = 0;
		}

		// таймаут вместо portMAX_DELAY, чтобы при пустой очереди успевать
		// заметить падение транспорта и снова уйти в буферизацию
		if (xQueueReceive(t->q, &m, pdMS_TO_TICKS(200)) == pdPASS)
		{
			switch (id)
			{
				case TR_USB:  usbprint(m.str);                break;
				case TR_MQTT: tr_send_mqtt(m.str);            break;
				case TR_OSC:  tr_send_osc(m.str);             break;
				case TR_UDP:  tr_send_udp(m.slot_num, m.str); break;
				default:                                      break;
			}

			heap_caps_free(m.str);
		}
	}
}

/* Положить отчёт в буфер транспорта. У каждого своя копия строки - каждый
   освобождает её сам, когда отправит. Транспорт не настроен (очереди нет) -
   молча игнорируем: ни копии, ни памяти. */
static void tr_post(tr_id_t id, const char * msg, int slot_num)
{
	transport_t * t = &s_tr[id];

	if (!t->q) return;

	reporter_message_t send_message;
	char *copy = heap_caps_malloc(strlen(msg)+1, MALLOC_CAP_8BIT);

	if (!copy)
	{
		ESP_LOGE(TAG, "reporter[%s] - malloc fail", t->name);
		return;
	}

	strcpy(copy, msg);
	send_message.str      = copy;
	send_message.slot_num = slot_num;

	if (xQueueSend(t->q, &send_message, 0) == pdPASS)
		return;

	// Буфер переполнен (транспорт лежит давно) - выкидываем самый старый отчёт
	// и кладём свежий. Так буфер всегда содержит актуальные события.
	reporter_message_t oldest;
	if (xQueueReceive(t->q, &oldest, 0) == pdPASS)
	{
		heap_caps_free(oldest.str);
		t->dropped++;
	}

	if (xQueueSend(t->q, &send_message, 0) != pdPASS)
		heap_caps_free(copy);   // не влезло даже после освобождения - сдаёмся

	// логируем переполнение, но не на каждый дроп, чтобы не забивать лог
	if (t->dropped == 1 || (t->dropped % 50) == 0)
		mblog(W, "reporter[%s] - buffer full, dropped %lu oldest reports",
				t->name, (unsigned long)t->dropped);
}

void forward_report(char *msg, int slot_num)
{
	/* По своему буферу на каждый настроенный транспорт, USB в том числе:
	   синхронный usbprint отсюда убран, иначе отчёт молча пропадал, пока хост
	   не перечислил устройство (см. комментарий у TR_USB). */
	for (tr_id_t id = 0; id < TR_COUNT; id++)
		tr_post(id, msg, slot_num);
}

void reporter_task(void *arg){
	reporter_message_t received_message;
	for(;;){
		if (xQueueReceive(me_state.reporter_queue, &received_message, portMAX_DELAY) == pdPASS){
			int len = strlen(received_message.str) + strlen(me_state.trigger_topic_list[received_message.slot_num]) + 6;
			char tmpStr[len];
			memset(tmpStr, 0, len);
			if(received_message.str[0]=='/'){
				sprintf(tmpStr,"%s%s", me_state.trigger_topic_list[received_message.slot_num], received_message.str);
			}else{
				sprintf(tmpStr,"%s:%s", me_state.trigger_topic_list[received_message.slot_num], received_message.str);
			}
			//ESP_LOGD(TAG, "Report: %s", tmpStr);

			forward_report(tmpStr, received_message.slot_num);
			crosslinker(tmpStr);

			// sometimes 5 ms lasting
			heap_caps_free(received_message.str);

			//vPortFree(received_message.str);
		}	
	}
}
void reporter_init(void){
	me_state.reporter_queue=xQueueCreate(150, sizeof(reporter_message_t));
	xTaskCreatePinnedToCore(reporter_task, "reporter_task", 1024 * 4, NULL, configMAX_PRIORITIES - 20, NULL, 0);
	//xTaskCreate (reporter_task, "reporter_task", 1024 * 4, NULL, configMAX_PRIORITIES - 8, NULL);

	/* Свой буфер и своя задача на каждый НАСТРОЕННЫЙ транспорт. Не настроен -
	   очередь не создаём: tr_post тогда молча игнорирует его, копий строк нет.
	   Сеть выключена целиком - не создаём ни одной. */
	for (tr_id_t id = 0; id < TR_COUNT; id++)
	{
		if (!tr_configured(id)) continue;

		s_tr[id].q = xQueueCreate(TR_QUEUE_DEPTH, sizeof(reporter_message_t));
		if (!s_tr[id].q)
		{
			ESP_LOGE(TAG, "reporter[%s] - queue alloc failed, transport disabled", s_tr[id].name);
			continue;
		}

		char taskName[24];
		snprintf(taskName, sizeof(taskName), "reporter_%s", s_tr[id].name);
		xTaskCreatePinnedToCore(transport_task, taskName, 1024 * 4,
								(void*)(intptr_t)id, configMAX_PRIORITIES - 20, NULL, 0);

		ESP_LOGD(TAG, "reporter[%s] - buffer %d created", s_tr[id].name, TR_QUEUE_DEPTH);
	}
}

void report(char *msg, int slot_num){
	//	memset(tmpStr, 0,64);
	//	sprintf(tmpStr, "monofonMSD\n");
	reporter_message_t send_message;

	//send_message.str = strdup(msg);

	char *copy = heap_caps_malloc(strlen(msg)+1, MALLOC_CAP_8BIT);
	if(!copy){
		ESP_LOGE(TAG, "malloc fail in report");
		return;
	}
	strcpy(copy, msg);
	send_message.str = copy;

	send_message.slot_num = slot_num;
	esp_err_t ret = xQueueSend(me_state.reporter_queue, &send_message, 5);
	//ESP_LOGD(TAG, "Set message:%s to report queue: %d", send_message.str, send_message.slot_num);
	if(ret!= pdPASS){
		ESP_LOGE(TAG, "QueueSend error:%d", ret);

		heap_caps_free(copy);
	}
	

	//free(send_message.str);
	//ESP_LOGD(TAG, "Set message:%s to report queue: %d", send_message.str, send_message.slot_num);
}

void reportFreeRAM(){
	char * tmpStr = heap_caps_malloc(128, MALLOC_CAP_8BIT);
	if(!tmpStr) return;
	snprintf(tmpStr, 128, "%s/system/event/freeRAM:%d",me_config.deviceName, xPortGetFreeHeapSize());
	forward_report(tmpStr, -1);
	heap_caps_free(tmpStr);
}

void reportFreeDisk(){
	char * tmpStr = heap_caps_malloc(128, MALLOC_CAP_8BIT);
	if(!tmpStr) return;
	uint64_t total = 0, free_bytes = 0;
	esp_err_t ret = esp_vfs_fat_info("/sdcard", &total, &free_bytes);
	if(ret == ESP_OK){
		snprintf(tmpStr, 128, "%s/system/event/freeDisk:%llu", me_config.deviceName, (unsigned long long)free_bytes);
	}else{
		snprintf(tmpStr, 128, "%s/system/event/freeDisk:error", me_config.deviceName);
	}
	forward_report(tmpStr, -1);
	heap_caps_free(tmpStr);
}

void reportVersion(){
	char * tmpStr = heap_caps_malloc(128, MALLOC_CAP_8BIT);
	if(!tmpStr) return;
	snprintf(tmpStr, 128, "%s/system/event/version:%s",me_config.deviceName, VERSION);
	forward_report(tmpStr, -1);
	heap_caps_free(tmpStr);
}

void reportNETstatus(){
	int bufSize = 768;
	char * tmpStr = heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
	if(!tmpStr) return;
	int pos = snprintf(tmpStr, bufSize, "%s/system/event/netStatus:{",me_config.deviceName);
	pos += snprintf(tmpStr + pos, bufSize - pos, "\"WIFI_init_res\":%d," , me_state.WIFI_init_res);
	if (me_state.WIFI_init_res == ESP_OK) {
		pos += snprintf(tmpStr + pos, bufSize - pos,
			"\"WIFI_SSID\":\"%s\",\"WIFI_ipAdress\":\"%s\",\"WIFI_netMask\":\"%s\",\"WIFI_gateWay\":\"%s\",",
			me_config.WIFI_ssid,
			me_config.WIFI_ipAdress,
			me_config.WIFI_netMask,
			me_config.WIFI_gateWay
		);
	}

	pos += snprintf(tmpStr + pos, bufSize - pos, "\"LAN_init_res\":%d," , me_state.LAN_init_res);
	if (me_state.LAN_init_res == ESP_OK) {
		pos += snprintf(tmpStr + pos, bufSize - pos,
			"\"LAN_ipAdress\":\"%s\",\"LAN_netMask\":\"%s\",\"LAN_gateWay\":\"%s\",",
			me_config.LAN_ipAdress,
			me_config.LAN_netMask,
			me_config.LAN_gateWay
		);
	}
	snprintf(tmpStr + pos, bufSize - pos,
		"\"MQTT_init_res\":%d,"
		"\"UDP_init_res\":%d,"
		"\"OSC_init_res\":%d,"
		"\"FTP_init_res\":%d}",
		me_state.MQTT_init_res,
		me_state.UDP_init_res,
		me_state.OSC_init_res,
		me_state.FTP_init_res
	);
	forward_report(tmpStr, -1);
	heap_caps_free(tmpStr);
}

void reportTaskList(){
	char * tmpStr = heap_caps_malloc(2048, MALLOC_CAP_8BIT);
	if(!tmpStr) return;
	int pos = snprintf(tmpStr, 2048, "%s/system/event/taskList:",me_config.deviceName);
	vTaskList(tmpStr + pos);
	forward_report(tmpStr, -1);
	heap_caps_free(tmpStr);
}

/* Считает количество открытых lwIP-сокетов (примерное верхнее значение —
   итерирует по всем возможным fd и проверяет состояние). Полезно для
   обнаружения утечек сокетов в multi-day uptime. */
static int count_open_sockets(void){
	int count = 0;
	int max_fd = LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS;
	for(int fd = LWIP_SOCKET_OFFSET; fd < max_fd; fd++){
		/* lwIP не реализует F_GETFD (флаги дескриптора), только F_GETFL
		   (статус-флаги сокета) — он же используется в audioLAN. F_GETFL
		   вернёт -1 (errno=EBADF) для закрытого/невалидного сокета. */
		int flags = fcntl(fd, F_GETFL, 0);
		if(flags >= 0) count++;
	}
	return count;
}

void reportSystemDiag(void){
	/* Пишем диагностический snapshot в SD-лог через mblog (не в MQTT).
	   Один JSON-объект, удобно грепать в /sdcard/log.txt. */
	const int bufSize = 640;
	char *tmpStr = heap_caps_malloc(bufSize, MALLOC_CAP_8BIT);
	if(!tmpStr) return;

	uint32_t heap_free      = xPortGetFreeHeapSize();
	uint32_t heap_min       = xPortGetMinimumEverFreeHeapSize();
	uint32_t heap_largest   = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
	uint32_t heap_internal  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
	uint32_t spiram_free    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

	int64_t now_us     = esp_timer_get_time();
	uint64_t uptime_s  = (uint64_t)(now_us / 1000000);

	mqtt_diag_t md;
	mqtt_diag_snapshot(&md);

	int64_t last_pub_age_s  = md.last_published_us  ? (now_us - md.last_published_us)  / 1000000 : -1;
	int64_t last_data_age_s = md.last_data_us       ? (now_us - md.last_data_us)       / 1000000 : -1;
	int64_t last_conn_age_s = md.last_connect_us    ? (now_us - md.last_connect_us)    / 1000000 : -1;
	int64_t last_disc_age_s = md.last_disconnect_us ? (now_us - md.last_disconnect_us) / 1000000 : -1;

	int socks = count_open_sockets();
	UBaseType_t tasks = uxTaskGetNumberOfTasks();

	snprintf(tmpStr, bufSize,
		"DIAG {"
		"\"up_s\":%llu,"
		"\"heap_free\":%lu,\"heap_min\":%lu,\"heap_largest\":%lu,"
		"\"heap_internal\":%lu,\"spiram_free\":%lu,"
		"\"socks\":%d,\"tasks\":%u,"
		"\"mqtt\":{"
			"\"up\":%u,"
			"\"conn\":%lu,\"disc\":%lu,\"pub\":%lu,\"data\":%lu,\"err\":%lu,"
			"\"pub_age_s\":%lld,\"data_age_s\":%lld,"
			"\"conn_age_s\":%lld,\"disc_age_s\":%lld"
		"}"
		"}",
		(unsigned long long)uptime_s,
		(unsigned long)heap_free, (unsigned long)heap_min, (unsigned long)heap_largest,
		(unsigned long)heap_internal, (unsigned long)spiram_free,
		socks, (unsigned)tasks,
		md.is_connected,
		(unsigned long)md.connected, (unsigned long)md.disconnected,
		(unsigned long)md.published, (unsigned long)md.data, (unsigned long)md.errors,
		(long long)last_pub_age_s, (long long)last_data_age_s,
		(long long)last_conn_age_s, (long long)last_disc_age_s
	);

	/* Используем W (warn), чтобы лог писался при дефолтном logLevel=warn,
	   без необходимости менять config.ini. */
	mblog(W, tmpStr);
	heap_caps_free(tmpStr);
}

void logTaskList(void){
	char *buf = heap_caps_malloc(2048, MALLOC_CAP_8BIT);
	if(!buf) return;
	/* Префикс "TASKS\n" чтобы блок было видно в логе. */
	strcpy(buf, "TASKS\n");
	vTaskList(buf + strlen(buf));
	mblog(W, buf);
	heap_caps_free(buf);
}
