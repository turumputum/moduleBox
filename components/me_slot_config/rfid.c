#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

//#include "pn532.h"
//#include "pn532_i2c_esp32.h"

#include "reporter.h"
#include "stateConfig.h"

#include "esp_log.h"
#include "me_slot_config.h"

#define PN532_I2C_ADDRESS 0x24
#define PN532_I2C_NUM I2C_NUM_0
#define PN532_SDA_GPIO 21
#define PN532_SCL_GPIO 22
#define PN532_PACKBUFFSIZ 64

//#define CONFIG_PN532DEBUG 1
// #define CONFIG_MIFAREDEBUG 1
extern uint8_t SLOTS_PIN_MAP[6][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "RFID";

int errorTick;

// void init_rfid_slot(int num_of_slot){
//     PN532 reader;

//     int res = PN532_I2C_Init(&reader, 5,4,0);   // For async command handling: >0 if response ready, 0 if not, -ve if error (e.g. no response expected)
//     ESP_LOGD(TAG, "RFID init res:%d", res);

//     vTaskDelay(pdMS_TO_TICKS(100));
//     res = PN532_SamConfiguration(&reader);
//     ESP_LOGD(TAG, "SAMConfig res:%d", res);
//     vTaskDelay(pdMS_TO_TICKS(100));
//     uint8_t versiondata[2];
//     //PN532_GetFirmwareVersion(&reader, versiondata);
//     if (PN532_GetFirmwareVersion(&reader, versiondata))
//     {
//         ESP_LOGE(TAG, "Could not find PN532");
//         //return;
//     }else{
//         ESP_LOGI(TAG, "Found PN532 with firmware version: %d.%d", versiondata[0], versiondata[1]);
//     }

    

//     while (1)
//     {
//         // Wait for an NFC card
//         uint8_t uid[7];
//         uint8_t uidLength;
//         uidLength = PN532_ReadPassiveTarget(&reader, uid, PN532_MIFARE_ISO14443A, 2000);
//         if(uidLength>0) {
//             ESP_LOGI(TAG, "Found card with UID:");
//             for (uint8_t i = 0; i < uidLength; i++)
//             {
//                 ESP_LOGI(TAG, "%02X ", uid[i]);
//             }
//             ESP_LOGI(TAG, "");
//         }
//         else
//         {
//             ESP_LOGI(TAG, "Waiting for card...");
//         }

//         // uint32_t versiondata = getPN532FirmwareVersion();
//         // if (! versiondata)
//         // {
//         //     ESP_LOGE(TAG, "Could not find PN532");
//         //     //return;
//         // }else{
//         //     ESP_LOGI(TAG, "Found PN532 with firmware version: %d.%d", (versiondata >> 24) & 0xFF, (versiondata >> 16) & 0xFF);
//         // }

//         vTaskDelay(1000 / portTICK_PERIOD_MS);
//     }
// }