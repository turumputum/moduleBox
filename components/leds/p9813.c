#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
//#include "freertos/queue.h"

#include "reporter.h"
#include "stateConfig.h"

#include "esp_system.h"
#include "esp_log.h"
#include "me_slot_config.h"

extern uint8_t SLOTS_PIN_MAP[6][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "P9813";
uint8_t *led_buf;
uint8_t clk_pin;
uint8_t data_pin;
uint8_t led_num;
uint8_t flag_led_write=0;

void p9813_set_led_color(uint8_t n, uint8_t r, uint8_t g, uint8_t b){
    led_buf[n*3]=r;
    led_buf[n*3+1]=g;
    led_buf[n*3+2]=b;
}

void p9813_write_led(){
    flag_led_write=1;
}

void p9813_send_byte(uint8_t byte){
    //printf("%d---", byte);
    for(uint8_t i = 0; i < 8; i++) {
        // If MSB is 1, write one and clock it, else write 0 and clock
        if ((byte & 0x80) != 0) {
            gpio_set_level(data_pin, 1); 
        } else {
            gpio_set_level(data_pin, 0);
        }
        gpio_set_level(clk_pin, 0);
        //-ets_delay_us(1);
        esp_rom_delay_us(1);
        gpio_set_level(clk_pin, 1);
        //-ets_delay_us(1);
        esp_rom_delay_us(1);
        byte <<= 1;
        //printf("%d", (byte & 0x80)!= 0);
    }
    //printf("\n");
}

void p9813_send_pixel(uint8_t num){
    uint8_t prefix = 0b11000000;
    uint8_t r = led_buf[num*3];
    if ((r & 0x80) == 0) {
        prefix |= 0b00000010;
    }
    if ((r & 0x40) == 0) {
        prefix |= 0b00000001;
    }
    uint8_t g = led_buf[num*3+1];
    if ((g & 0x80) == 0) {
        prefix |= 0b00001000;
    }
    if ((g & 0x40) == 0) {
        prefix |= 0b00000100;
    }
    uint8_t b = led_buf[num*3+2];
    if ((b & 0x80) == 0) {
        prefix |= 0b00100000;
    }
    if ((b & 0x40) == 0) {
        prefix |= 0b00010000;
    }
    p9813_send_byte(prefix);
    p9813_send_byte(b);
    p9813_send_byte(g);
    p9813_send_byte(r);
    //ESP_LOGD(TAG, "Led write pixel:%d r:%d g:%d b:%d", num, r, g, b);
}


void p9813_sender_task(){
    //ESP_LOGD(TAG, "Led sender task started");
    while(1){
        if(flag_led_write){
            flag_led_write=0;
            for(int i=0; i<4; i++){
                p9813_send_byte(0x00);
            }
            for(int i=0; i<led_num; i++){
                p9813_send_pixel(i);
            }
            for(int i=0; i<4; i++){
                p9813_send_byte(0x00);
            }
            //ESP_LOGD(TAG, "Led write end");
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


void init_p9813(int slot_num, int l_num){
    clk_pin = SLOTS_PIN_MAP[slot_num][0];
    data_pin = SLOTS_PIN_MAP[slot_num][1];
    //-pio_pad_select_gpio(clk_pin);
    esp_rom_gpio_pad_select_gpio(clk_pin);
    gpio_set_direction(clk_pin, GPIO_MODE_OUTPUT);
    //-gpio_pad_select_gpio(data_pin);
    esp_rom_gpio_pad_select_gpio(clk_pin);
    gpio_set_direction(data_pin, GPIO_MODE_OUTPUT);

    led_num = l_num;
    //TO DO rewrite global vars
    led_buf =(uint8_t *) malloc(led_num*3);

    xTaskCreate(p9813_sender_task, "p9813_sender_task", 1024*4, NULL,5, NULL);
    ESP_LOGD(TAG, "Led inited");
}







