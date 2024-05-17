#include <stdlib.h>
#include "tusb.h"
#include "myHID.h"
#include "class/hid/hid_device.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"


#include "reporter.h"
#include "executor.h"
#include "stateConfig.h"

extern stateStruct me_state;
extern configuration me_config;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char* TAG = "myHID";

uint8_t pressed_count = 0;
uint8_t keycodes[6]={0};


// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
}

void send_hid_key_press(uint8_t key)
{
    //ESP_LOGI(TAG, "Sending Keyboard pressed");
    keycodes[pressed_count] = key;
    pressed_count++;
    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, keycodes);
}
void send_hid_key_release(uint8_t key)
{
    //ESP_LOGI(TAG, "Sending Keyboard released");
//     uint8_t keycode[6] = {HID_KEY_A + key};
    for(int i=0; i<pressed_count; i++){
        if(keycodes[i] == key){
            if(i<pressed_count-1){
                for(int j=i; j<pressed_count-1; j++){
                    keycodes[j] = keycodes[j+1];
                }
            }else{
                keycodes[i] = 0;
            }
            break;
        }
    }
    pressed_count--;
    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, keycodes);
}

void HID_task(void *arg) {
    int slot_num = *(int*) arg;
    command_message_t msg;
    me_state.command_queue[slot_num] = xQueueCreate(5, sizeof(command_message_t));

    char t_str[strlen(me_config.device_name)+strlen("/HID_0")+3];
    sprintf(t_str, "%s/HID_%d",me_config.device_name, slot_num);
    me_state.action_topic_list[slot_num]=strdup(t_str);
    ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
    ESP_LOGD(TAG, "HID task init ok. Slot_num:%d", slot_num);

    while(1)   {
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            cmd = cmd + strlen(me_state.action_topic_list[slot_num]);
            if(strstr(cmd, "press")!=NULL){
                send_hid_key_press(atoi(payload));
            }else if(strstr(cmd, "release")!=NULL){
                send_hid_key_release(atoi(payload));
            }else if(strstr(cmd, "kick")!=NULL){
                send_hid_key_press(atoi(payload));
                vTaskDelay(pdMS_TO_TICKS(50));
                send_hid_key_release(atoi(payload));
            }else{
                ESP_LOGD(TAG, "Unknown command %s", cmd);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void start_HID_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
	//char tmpString[strlen("HID_task")];
	//sprintf(tmpString,"%s", "HID_task");
    char tmpString = strdup("HID_task");
	xTaskCreatePinnedToCore(HID_task, "HID_task", 1024*4, &slot_num,configMAX_PRIORITIES - 12, NULL, 0);
    ESP_LOGD(TAG, "HID_task init ok. Heap usage: %lu free heap:%u", heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}