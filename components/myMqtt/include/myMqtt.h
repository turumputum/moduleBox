#ifndef __MYMQTT_H__
#define __MYMQTT_H__

#include <stdint.h>

int mqtt_app_start(void);
void mqtt_pub(const char *topic, const char *string);
void mqtt_pub_retain(const char *topic, const char *string);
void mqtt_sub(const char *topic);

/* Диагностика MQTT-клиента — счётчики накапливаются обработчиком событий.
   Используется reporter'ом для периодической публикации <dev>/system/diag. */
typedef struct {
    uint32_t connected;          /* MQTT_EVENT_CONNECTED count */
    uint32_t disconnected;       /* MQTT_EVENT_DISCONNECTED count */
    uint32_t published;          /* MQTT_EVENT_PUBLISHED count (transport-out) */
    uint32_t data;               /* MQTT_EVENT_DATA count (inbound subs)        */
    uint32_t errors;             /* MQTT_EVENT_ERROR count */
    int64_t  last_published_us;  /* esp_timer_get_time() at last PUBLISHED */
    int64_t  last_data_us;       /* esp_timer_get_time() at last DATA      */
    int64_t  last_connect_us;    /* esp_timer_get_time() at last CONNECTED */
    int64_t  last_disconnect_us; /* esp_timer_get_time() at last DISCONNECTED */
    uint8_t  is_connected;       /* current connection state (set by handler) */
} mqtt_diag_t;

/* Возвращает снимок счётчиков (потокобезопасно — копирует под critical-section'ом). */
void mqtt_diag_snapshot(mqtt_diag_t *out);

#endif
