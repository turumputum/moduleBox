#include <stdio.h>
#include "CRSF.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <driver/uart.h>

#include "executor.h"
#include "reporter.h"
#include "stateConfig.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "CRSF";

//------------------------------CRSF_RX_TASK---------------------------------

typedef struct {
    uint8_t deviceAddr;
    uint8_t frameLength;
    uint8_t type;
    uint8_t payload[32];
    uint8_t crc;
} crsf_frame_t;

static void UnpackChannels(uint8_t const * const payload, int32_t * const dest){
    const unsigned numOfChannels = 16;
    const unsigned srcBits = 11;
    //const unsigned dstBits = 32;
    const unsigned inputChannelMask = (1 << srcBits) - 1;

    // code from BetaFlight rx/crsf.cpp / bitpacker_unpack
    uint8_t bitsMerged = 0;
    int32_t readValue = 0;
    unsigned readByteIndex = 0;
    for (uint8_t n = 0; n < numOfChannels; n++)
    {
        while (bitsMerged < srcBits)
        {
            uint8_t readByte = payload[readByteIndex++];
            readValue |= ((int32_t) readByte) << bitsMerged;
            bitsMerged += 8;
        }
        //printf("rv=%x(%x) bm=%u\n", readValue, (readValue & inputChannelMask), bitsMerged);
        dest[n] = (readValue & inputChannelMask);
        if(dest[n] > 2048)dest[n] = 2048;
        if(dest[n] < 0)dest[n] = 0;
        readValue >>= srcBits;
        bitsMerged -= srcBits;
    }
}


void crsf_rx_task(void* arg) {
    int slot_num = *(int*)arg;
    crsf_frame_t frame;

    uint16_t minVal = 0;
    uint16_t maxVal = 1984;

    uint16_t deadBand = 1;
	if (strstr(me_config.slot_options[slot_num], "deadBand") != NULL) {
		deadBand = get_option_int_val(slot_num, "deadBand", "", 10, 1, 4096);
		ESP_LOGD(TAG, "Set deadBand:%d for slot:%d",deadBand, slot_num);
	}

    if (strstr(me_config.slot_options[slot_num], "topic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "topic", "/CRSF_0");
		me_state.trigger_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/CRSF_0")+3];
		sprintf(t_str, "%s/CRSF_%d",me_config.deviceName, slot_num);
		me_state.trigger_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart trigger_topic:%s", me_state.trigger_topic_list[slot_num]);
	}
    
    // Find available UART port
    int uart_num = UART_NUM_1;
    while (uart_is_driver_installed(uart_num)) {
        uart_num++;
        if (uart_num >= UART_NUM_MAX) {
            ESP_LOGE(TAG, "slot num:%d ___ No free UART driver", slot_num);
            vTaskDelete(NULL);
        }
    }

    // Configure UART pins
    uint8_t tx_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t rx_pin = SLOTS_PIN_MAP[slot_num][1];

    // CRSF UART config (420000 baud, 8N1)
    uart_config_t uart_config = {
        .baud_rate = 420000,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_param_config(uart_num, &uart_config);
    gpio_reset_pin(tx_pin);
    gpio_reset_pin(rx_pin);
    uart_set_pin(uart_num, tx_pin, rx_pin, -1, -1);
    uart_driver_install(uart_num, 256, 256, 0, NULL, 0);

    // CRSF frame buffer
    //uint8_t buffer[64];
    uint8_t numOfChannel = 16;
    int32_t channels[numOfChannel];
    memset (channels, ((maxVal-minVal)/2)+minVal, sizeof(channels));
    // In the crsf_rx_task:

    waitForWorkPermit(slot_num);

    while(1) {
        uint8_t buffer[64];
        crsf_frame_t frame;
        
        // Wait for CRSF header (0xC8)
        uint8_t byte;
        if(uart_read_bytes(uart_num, &byte, 1, pdMS_TO_TICKS(100)) > 0) {
            if(byte == 0xC8) {
                buffer[0] = byte;
                frame.deviceAddr = byte;
                
                // Read frame length
                if(uart_read_bytes(uart_num, &byte, 1, pdMS_TO_TICKS(1)) > 0) {
                    buffer[1] = byte;
                    frame.frameLength = byte;
                    
                    // Read type and payload
                    if(uart_read_bytes(uart_num, &buffer[2], frame.frameLength, pdMS_TO_TICKS(1)) == frame.frameLength) {
                        frame.type = buffer[2];
                        
                        // Extract payload
                        uint8_t payload_len = frame.frameLength - 2; // Subtract type and CRC
                        memcpy(frame.payload, &buffer[3], payload_len);
                        
                        // Get CRC
                        frame.crc = buffer[frame.frameLength + 1];
                        
                        // Parse RC channels data if type is 0x16
                        if(frame.type == 0x16) {
                            //ESP_LOGD(TAG, "Received payload: %x %x %x %x %x %x", frame.payload[0], frame.payload[1], frame.payload[2], frame.payload[3], frame.payload[4], frame.payload[5]);
                            int32_t rawChannels[numOfChannel];
                            UnpackChannels(&frame.payload, rawChannels);
                            for(int i=0; i<8; i++){
                                if(abs(rawChannels[i]-channels[i])>deadBand){
                                   
                                    char str[255];
                                    memset(str, 0, sizeof(str));
                                    //sprintf(str, "/ch_%d:%d", i,(uint8_t)(fVal*255));
                                    sprintf(str, "/ch_%d:%ld", i, rawChannels[i]);
                                    report(str, slot_num);
                                    //ESP_LOGD(TAG, "report chan:%d rawVal:%ld", i, rawChannels[i]);
                                    channels[i] = rawChannels[i];
                                }
                            }
                            // Now channels array contains 16 RC channel values
                            // Each channel is 11 bits
                            //ESP_LOGD(TAG, "CH1: %ld, CH2: %ld, CH3: %ld, CH4: %ld, CH5: %ld, CH6: %ld, CH7: %ld, CH8: %ld ",channels[0], channels[1], channels[2], channels[3], channels[4], channels[5], channels[6], channels[7]);
                        }
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15)); // Prevent watchdog trigger
    }
}


void start_crsf_rx_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    int t_slot_num = slot_num;
	char tmpString[60];
	sprintf(tmpString, "crsf_rx_task_%d", slot_num);
	xTaskCreatePinnedToCore(crsf_rx_task, tmpString, 1024*4, &t_slot_num,configMAX_PRIORITIES - 12, NULL, 1);
    ESP_LOGD(TAG, "crsf_rx_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
