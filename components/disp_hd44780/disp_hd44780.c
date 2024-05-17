#include <stdio.h>
#include "disp_hd44780.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "executor.h"
#include "reporter.h"
#include "stateConfig.h"


#include "me_slot_config.h"


#define LCD_RS_PIN 1//0
#define LCD_EN_PIN 4//2
#define LCD_D4_PIN 4
#define LCD_D5_PIN 5 
#define LCD_D6_PIN 6
#define LCD_D7_PIN 7


extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "DISP_HD44780";


static void LCD_pulseEnable(uint8_t data){
    uint8_t write_buf[1];
    write_buf[0] = data | LCD_ENABLE;
    i2c_master_write_to_device(I2C_NUM_0, LCD_ADDR, write_buf, sizeof(write_buf), 1000 / portTICK_PERIOD_MS);
    vTaskDelay(1/ portTICK_PERIOD_MS);

    write_buf[0] = data & ~LCD_ENABLE;
    i2c_master_write_to_device(I2C_NUM_0, LCD_ADDR, write_buf, sizeof(write_buf), 1000 / portTICK_PERIOD_MS);
    vTaskDelay(1/ portTICK_PERIOD_MS);
}

static void LCD_writeNibble(uint8_t nibble, uint8_t mode){
    uint8_t data = (nibble & 0xF0) | mode | LCD_BACKLIGHT;
    i2c_master_write_to_device(I2C_NUM_0, LCD_ADDR, &data, 1, 1000 / portTICK_PERIOD_MS);  

    LCD_pulseEnable(data);

}

static void LCD_writeByte(uint8_t data, uint8_t mode){
    LCD_writeNibble(data & 0xF0, mode);
    LCD_writeNibble((data << 4) & 0xF0, mode);
}

void LCD_writeChar(char c){
    LCD_writeByte(c, LCD_WRITE);                                        // Write data to DDRAM
}

void LCD_writeStr(char* str){
    while (*str) {
        LCD_writeChar(*str++);
    }
}

void LCD_home(void){
    LCD_writeByte(LCD_HOME, LCD_COMMAND);
    vTaskDelay(2 / portTICK_RATE_MS);                                   // This command takes a while to complete
}

void LCD_clearScreen(void){
    LCD_writeByte(LCD_CLEAR, LCD_COMMAND);
    vTaskDelay(2 / portTICK_RATE_MS);                                   // This command takes a while to complete
}


void LCD_setCursor(uint8_t col, uint8_t row){
    if (row > LCD_ROWS - 1) {
        ESP_LOGE(TAG, "Cannot write to row %d. Please select a row in the range (0, %d)", row, LCD_ROWS-1);
        row = LCD_ROWS - 1;
    }
    uint8_t row_offsets[] = {LCD_LINEONE, LCD_LINETWO, LCD_LINETHREE, LCD_LINEFOUR};
    LCD_writeByte(LCD_SET_DDRAM_ADDR | (col + row_offsets[row]), LCD_COMMAND);
}

void disp_hd44780_task(void* arg) {
    int slot_num = *(int*)arg;

    me_state.command_queue[slot_num] = xQueueCreate(10, sizeof(command_message_t));

    uint8_t SDA_pin = SLOTS_PIN_MAP[slot_num][0];
    uint8_t SCL_pin = SLOTS_PIN_MAP[slot_num][1];

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_pin,
        .scl_io_num = SCL_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000
    };
	i2c_param_config(I2C_NUM_0, &conf);
	i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);

    // Reset the LCD controller
    LCD_writeNibble(LCD_FUNCTION_RESET, LCD_COMMAND);                   // First part of reset sequence
    vTaskDelay(4 / portTICK_RATE_MS);                                  // 4.1 mS delay (min)
    LCD_writeNibble(LCD_FUNCTION_RESET, LCD_COMMAND);                   // second part of reset sequence
    vTaskDelay(1 );                                                     // 100 uS delay (min)
    LCD_writeNibble(LCD_FUNCTION_RESET, LCD_COMMAND);                   // Third time's a charm
    LCD_writeNibble(LCD_FUNCTION_SET_4BIT, LCD_COMMAND);                // Activate 4-bit mode
     vTaskDelay(1 );                                                   // 40 uS delay (min)

    // --- Busy flag now available ---
    // Function Set instruction
    LCD_writeByte(LCD_FUNCTION_SET_4BIT, LCD_COMMAND);                  // Set mode, lines, and font
     vTaskDelay(1/ portTICK_RATE_MS);

    // Clear Display instruction
    LCD_writeByte(LCD_CLEAR, LCD_COMMAND);                              // clear display RAM
    vTaskDelay(2 / portTICK_RATE_MS);                                   // Clearing memory takes a bit longer
    
    // Entry Mode Set instruction
    LCD_writeByte(LCD_ENTRY_MODE, LCD_COMMAND);                         // Set desired shift characteristics
     vTaskDelay(1 / portTICK_RATE_MS); 

    LCD_writeByte(LCD_DISPLAY_ON, LCD_COMMAND);                         // Ensure LCD is set to on


    char t_str[strlen(me_config.device_name)+strlen("/disp_0")+3];
    sprintf(t_str, "%s/disp_%d",me_config.device_name, slot_num);
    me_state.action_topic_list[slot_num]=strdup(t_str);
    ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);

    //uint32_t count=0;
    LCD_home();
    LCD_clearScreen();
    // LCD_setCursor(15-5, 0);
    // LCD_writeStr("MAMBA");
    while (1){
        command_message_t msg;
        if (xQueueReceive(me_state.command_queue[slot_num], &msg, 0) == pdPASS){
            char* payload;
            char* cmd = strtok_r(msg.str, ":", &payload);
            ESP_LOGD(TAG, "Input command %s payload:%s", cmd, payload);
            if(strlen(payload)>16){
                char str[17];
                strncpy(&str, payload, 16);
                str[16]='\0';
                char str2[17];
                char* str2_ptr = &payload[16];
                strncpy(&str2, str2_ptr, 16);
                str2[16]='\0';
                LCD_setCursor(0, 0);
                LCD_writeStr(str);
                LCD_setCursor(0, 1);
                LCD_writeStr(str2);
            }else{
                LCD_setCursor(0, 0);
                LCD_writeStr(payload);
            }
        }
        vTaskDelay(50 / portTICK_RATE_MS);
    }
}


void start_disp_hd44780_task(int slot_num) {
    uint32_t heapBefore = xPortGetFreeHeapSize();
    xTaskCreate(disp_hd44780_task, "disp_hd44780_task", 1024 * 4, &slot_num, configMAX_PRIORITIES-12, NULL);
    ESP_LOGD(TAG, "disp_hd44780_task init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}