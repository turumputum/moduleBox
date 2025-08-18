#include "smartLed.h"
//#include "led_strip.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_check.h"
#include "driver/rmt_tx.h"

#include "reporter.h"
#include "stateConfig.h"
#include "executor.h"

#include "esp_log.h"
#include "me_slot_config.h"

#include "math.h"

#include "rgbHsv.h"

#include <stdcommand.h>

#include <generated_files/gen_smartLed.h>


// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_SMARTLEDCONFIG
{
    uint16_t                num_of_led;
    uint8_t                 inverse;
    uint8_t                 state;
    int16_t                 increment;
    int16_t                 maxBright;
    int16_t                 minBright;
    uint16_t                refreshPeriod;
    RgbColor                targetRGB;
    int                     ledMode;
    int                     floatReport;
    int                     numOfPos;
    uint16_t                effectLen;
    int                     dir;
    int16_t                 offset;
    STDCOMMANDS             cmds;
} SMARTLEDCONFIG, * PSMARTLEDCONFIG; 

typedef enum
{
    MYCMD_default = 0,
    MYCMD_setRGB,
    MYCMD_setMode,
    MYCMD_setIncrement,
    MYCMD_setPos,
    MYCMD_swipe
} MYCMD;

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

//#define LED_STRIP_RMT_DEFAULT_MEM_BLOCK_SYMBOLS 48

#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "SMART_LED";

extern const uint8_t gamma_8[256];

SemaphoreHandle_t rmt_semaphore=NULL;

#define MAX_CHANNELS 3
uint8_t rmt_chan_counter = 0;



void led_strip_set_pixel(uint8_t *pixel_array, int pos, int r, int g, int b){
    pixel_array[pos * 3 + 0]= (uint8_t)g;
    pixel_array[pos * 3 + 1]= (uint8_t)r;
    pixel_array[pos * 3 + 2]= (uint8_t)b;
}

void setAllLed_color(uint8_t *pixel_array, RgbColor color, int16_t bright, uint16_t num_of_led){
    float fbright = (float)bright/255.0;
    uint8_t R = color.r*fbright;
    uint8_t G = color.g*fbright;
    uint8_t B = color.b*fbright;
    //ESP_LOGD(TAG, "setAllLed_color RGB:%d,%d,%d", R,G,B);
    for(int i=0; i<num_of_led; i++){
        led_strip_set_pixel(pixel_array, i, R,G,B);
    }
}

static size_t rmt_encode_led_strip(rmt_encoder_t *encoder, rmt_channel_handle_t channel, const void *primary_data, size_t data_size, rmt_encode_state_t *ret_state){
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_handle_t bytes_encoder = led_encoder->bytes_encoder;
    rmt_encoder_handle_t copy_encoder = led_encoder->copy_encoder;
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;
    switch (led_encoder->state) {
    case 0: // send RGB data
        encoded_symbols += bytes_encoder->encode(bytes_encoder, channel, primary_data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1; // switch to next state when current encoding session finished
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out; // yield if there's no free space for encoding artifacts
        }
    // fall-through
    case 1: // send reset code
        encoded_symbols += copy_encoder->encode(copy_encoder, channel, &led_encoder->reset_code,
                                                sizeof(led_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET; // back to the initial encoding session
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out; // yield if there's no free space for encoding artifacts
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t rmt_del_led_strip_encoder(rmt_encoder_t *encoder){
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_del_encoder(led_encoder->bytes_encoder);
    rmt_del_encoder(led_encoder->copy_encoder);
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t rmt_led_strip_encoder_reset(rmt_encoder_t *encoder){
    rmt_led_strip_encoder_t *led_encoder = __containerof(encoder, rmt_led_strip_encoder_t, base);
    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

esp_err_t rmt_new_led_strip_encoder(const led_strip_encoder_config_t *config, rmt_encoder_handle_t *ret_encoder){
    esp_err_t ret = ESP_OK;
    rmt_led_strip_encoder_t *led_encoder = NULL;
    ESP_GOTO_ON_FALSE(config && ret_encoder, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    led_encoder = calloc(1, sizeof(rmt_led_strip_encoder_t));
    ESP_GOTO_ON_FALSE(led_encoder, ESP_ERR_NO_MEM, err, TAG, "no mem for led strip encoder");
    led_encoder->base.encode = rmt_encode_led_strip;
    led_encoder->base.del = rmt_del_led_strip_encoder;
    led_encoder->base.reset = rmt_led_strip_encoder_reset;
    // different led strip might have its own timing requirements, following parameter is for WS2812
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 0.3 * config->resolution / 1000000, // T0H=0.3us
            .level1 = 0,
            .duration1 = 0.9 * config->resolution / 1000000, // T0L=0.9us
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 0.9 * config->resolution / 1000000, // T1H=0.9us
            .level1 = 0,
            .duration1 = 0.3 * config->resolution / 1000000, // T1L=0.3us
        },
        .flags.msb_first = 1 // WS2812 transfer bit order: G7...G0R7...R0B7...B0
    };
    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_encoder_config, &led_encoder->bytes_encoder), err, TAG, "create bytes encoder failed");
    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_config, &led_encoder->copy_encoder), err, TAG, "create copy encoder failed");

    uint32_t reset_ticks = config->resolution / 1000000 * 300 / 2; // reset code duration defaults to 50us
    led_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = reset_ticks,
        .level1 = 0,
        .duration1 = reset_ticks,
    };
    *ret_encoder = &led_encoder->base;
    return ESP_OK;
err:
    if (led_encoder) {
        if (led_encoder->bytes_encoder) {
            rmt_del_encoder(led_encoder->bytes_encoder);
        }
        if (led_encoder->copy_encoder) {
            rmt_del_encoder(led_encoder->copy_encoder);
        }
        free(led_encoder);
    }
    return ret;
}

uint8_t rmt_createAndSend(rmt_led_heap_t *rmt_slot_heap, uint8_t *led_strip_pixels, uint16_t size, uint8_t slot_num){
    if (xSemaphoreTake(rmt_semaphore, portMAX_DELAY) == pdTRUE) {
        //ESP_LOGD(TAG, "rmt_slot_heap->->resolution_hz:%ld, rmt_slot_heap->->trans_queue_depth:%d", rmt_slot_heap->tx_chan_config.resolution_hz,rmt_slot_heap->tx_chan_config.trans_queue_depth);
        //ESP_LOGD(TAG, "sizeof(led_strip_pixels):%d", sizeof(led_strip_pixels));
        //rmt_slot_heap->led_chan->channel_id = uxSemaphoreGetCount(rmt_semaphore);
        int fail_count = 0;
        while(rmt_new_tx_channel(&rmt_slot_heap->tx_chan_config, &rmt_slot_heap->led_chan)!= ESP_OK){
            ESP_LOGE(TAG, "lets repeat creat RMT TX channel for slot:%d", slot_num);
            //ESP_LOGD(TAG, "rmt_semaphore_count:%d", uxSemaphoreGetCount(rmt_semaphore));
            fail_count++;
            if(fail_count>10){
                ESP_LOGE(TAG, "RMT TX channel fail for slot:%d", slot_num);
                return ESP_FAIL;
            }
        }
        rmt_enable(rmt_slot_heap->led_chan);
        esp_err_t err = rmt_transmit(rmt_slot_heap->led_chan, rmt_slot_heap->led_encoder, led_strip_pixels, size, &rmt_slot_heap->tx_config);
        if(err!=ESP_OK){
            ESP_LOGE(TAG, "RMT TX error:%d", err);
        }
        if(rmt_tx_wait_all_done(rmt_slot_heap->led_chan, portMAX_DELAY)!=ESP_OK){
            ESP_LOGE(TAG, "RMT TX wait error:%d", err);
        }
        if(rmt_disable(rmt_slot_heap->led_chan)!=ESP_OK){
            ESP_LOGE(TAG, "RMT TX disable error:%d", err);
        }
        if(rmt_del_channel(rmt_slot_heap->led_chan)!=ESP_OK){
            ESP_LOGE(TAG, "RMT TX del error:%d", err);
        }
        gpio_set_direction(rmt_slot_heap->tx_chan_config.gpio_num, GPIO_MODE_OUTPUT);
        gpio_set_level(rmt_slot_heap->tx_chan_config.gpio_num, 0);
        //vTaskDelay(pdMS_TO_TICKS(1));
        
    }else{
        ESP_LOGE(TAG, "RMT semaphore fail for slot:%d", slot_num);
    }
    xSemaphoreGive(rmt_semaphore);
    return ESP_OK;
}




//---------------------LED_STRIP----------------------------

void init_runEffect(uint8_t* led_strip_pixels, uint16_t numOfLed, uint8_t minBright, uint8_t maxBright, RgbColor* color) {
    float range = (float)abs(maxBright - minBright)/255;
    for (int i = 0; i < numOfLed; i++) {
        float phase = i * M_PI / (numOfLed-1);
        float value = (float)(sin(phase))*range;
        if(value < 0)value = 0.0;
        value = value + (float)minBright/255;
        if(value > 1)value = 1.0;
        led_strip_set_pixel(led_strip_pixels, i, gamma_8[(uint8_t)(color->r*value)],gamma_8[(uint8_t)(color->g*value)],gamma_8[(uint8_t)(color->b*value)]);
        //led_strip_set_pixel(led_strip_pixels, i, color->r*value,color->g*value,color->b*value);
        // ESP_LOGD(TAG, "i:%d phase:%f value:%f range:%f", i, phase, value, range);
        //printf("%d-", swiperLed->effectBuf[i]);
    }
    // for(int i=0;i<numOfLed;i++){
    //     printf("%d-", led_strip_pixels[i*3+2]);
    // }
    // printf("\n");
}
/*
    Модуль поддержки Smart-LED
*/
void configure_button_smartLed(PSMARTLEDCONFIG c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* Количенство светодиодов
    */
    c->num_of_led = get_option_int_val(slot_num, "numOfLed", "", 24, 1, 1024);
    ESP_LOGD(TAG, "Set num_of_led:%d for slot:%d",c->num_of_led, slot_num);

    /* Флаг задаёт инвертирование значений 
    */
    c->inverse = get_option_flag_val(slot_num, "ledInverse");

    /* Состояние по умолчанию
    */
    c->state = get_option_int_val(slot_num, "defaultState", "", 0, 0, 1);
    ESP_LOGD(TAG, "Set def_state:%d for slot:%d", c->state, slot_num);
        
    /* Величина приращения
    */
    c->increment = get_option_int_val(slot_num, "increment", "", 255, 0, 255);
    if(c->increment<1)c->increment=1;
    if(c->increment>255)c->increment=255;
    ESP_LOGD(TAG, "Set increment:%d for slot:%d", c->increment, slot_num);

    /* Максимальное значение яркости
    */
    c->maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 4095);
    if(c->maxBright>255)c->maxBright=255;
    if(c->maxBright<0)c->maxBright=0;
    ESP_LOGD(TAG, "Set maxBright:%d for slot:%d", c->maxBright, slot_num);

    /* Минимальное значение яркости
    */
    c->minBright = get_option_int_val(slot_num, "minBright", "", 0, 0, 4095);
    if(c->minBright<0)c->minBright=0;
    if(c->minBright>255)c->minBright=255;
    ESP_LOGD(TAG, "Set minBright:%d for slot:%d", c->minBright, slot_num);

    /* Период обновления 
    */
    c->refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 1000/30, 1, 4096));
    ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d", c->refreshPeriod, slot_num);
    	
    /* Начальный цвет
    */
    if (get_option_color_val(&c->targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }
    ESP_LOGD(TAG, "Set color:%d %d %d for slot:%d", c->targetRGB.r, c->targetRGB.g, c->targetRGB.b, slot_num);

    /* Задаёт режим анимации */
    if ((c->ledMode = get_option_enum_val(slot_num, "ledMode", "default", "flash", "glitch", "swiper", "rainbow", "run", NULL)) < 0)
    {
        ESP_LOGE(TAG, "ledMode: unricognized value");
    }
    ESP_LOGD(TAG, "Set ledMode:%d for slot:%d", c->ledMode, slot_num);

    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
        /* Определяет топик для MQTT сообщений */
    	custom_topic = get_option_string_val(slot_num, "ledTopic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/smartLed_0")+3];
		sprintf(t_str, "%s/smartLed_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    /* Числовое значение.
       задаёт текущее состояние светодиода (вкл/выкл)
    */
    stdcommand_register(&c->cmds, MYCMD_default, NULL, PARAMT_int);

    /* Установить новый целевой цвет. 
       Цвет задаётся десятичными значениями R G B через пробел
    */
    stdcommand_register(&c->cmds, MYCMD_setRGB, "setRGB", PARAMT_int, PARAMT_int, PARAMT_int);

    /* Установить новый режим анимации цветов
    */
    stdcommand_register_enum(&c->cmds, MYCMD_setMode, "setMode", "default", "flash", "glitch", "swiper", "rainbow", "run");

    /* Установить новое значение приращения
    */
    stdcommand_register(&c->cmds, MYCMD_setIncrement, "setIncrement", PARAMT_int);
    
}
void smartLed_task(void *arg){
    //PSMARTLEDCONFIG c = calloc(1, sizeof(SMARTLEDCONFIG));
    SMARTLEDCONFIG c_struct = {0};
    PSMARTLEDCONFIG c = &c_struct;
    uint32_t startTick = xTaskGetTickCount();
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
    uint8_t flag_ledUpdate = 0;
    STDCOMMAND_PARAMS       params = { 0 };

	me_state.command_queue[slot_num] = xQueueCreate(10, sizeof(command_message_t));
    
    if(rmt_semaphore==NULL){
        rmt_semaphore = xSemaphoreCreateCounting(1, 1);
    }

    configure_button_smartLed(c, slot_num);

    uint8_t led_strip_pixels[c->num_of_led * 3];

    if(c->ledMode==MODE_RUN){
        init_runEffect(&led_strip_pixels[0], c->num_of_led, c->minBright, c->maxBright, &c->targetRGB);
    }


    rmt_led_heap_t rmt_slot_heap = RMT_LED_HEAP_DEFAULT();
    rmt_slot_heap.tx_chan_config.gpio_num = pin_num;

    //rmt_slot_heap.tx_chan_config.flags.io_od_mode = true;
    rmt_new_led_strip_encoder(&rmt_slot_heap.encoder_config, &rmt_slot_heap.led_encoder);

    ESP_LOGD(TAG, "Smart led task config end. Slot_num:%d, duration_ms:%ld", slot_num, pdTICKS_TO_MS(xTaskGetTickCount()-startTick));

    int16_t currentBright=0;
    int16_t targetBright=c->minBright;
    RgbColor currentRGB={
        .r=0,
        .g=0,
        .b=0
    };
    //state = inverse;
    TickType_t lastWakeTime = xTaskGetTickCount(); 
    TickType_t lastUpdateTime = xTaskGetTickCount();

    waitForWorkPermit(slot_num);

    while (1) {
        switch (stdcommand_receive(&c->cmds, &params, 0))
        {
            case -1: // none
                break;

            case MYCMD_default:
                c->state = params.p[0].i;
                if(c->ledMode==MODE_RUN){
                    init_runEffect(&led_strip_pixels[0], c->num_of_led, c->minBright, c->maxBright, &c->targetRGB);
                }
                if(c->state==0){
                    currentBright = targetBright-1;
                }
                ESP_LOGD(TAG, "Slot:%d Change state to:%d freeHeap:%d",slot_num, c->state, xPortGetFreeHeapSize());
                break;

            case MYCMD_setRGB:
                c->targetRGB.r = params.p[0].i;
                c->targetRGB.g = params.p[1].i;
                c->targetRGB.b = params.p[2].i;

                ESP_LOGD(TAG, "Slot:%d target RGB: %d %d %d", slot_num, c->targetRGB.r, c->targetRGB.g, c->targetRGB.b); 
                break;

            case MYCMD_setMode:
                c->ledMode = params.enumResult;
                
                if(c->ledMode==MODE_RUN)
                {
                    ESP_LOGD(TAG, "Slot:%d init run effect", slot_num); 

                    init_runEffect(&led_strip_pixels[0], c->num_of_led, c->minBright, c->maxBright, &c->targetRGB);
                }
                break;

            case MYCMD_setIncrement:
                c->increment = params.p[0].i;
                ESP_LOGD(TAG, "Set fade increment:%d", c->increment);
                break;


            default:
                //ESP_LOGD(TAG, "@@@@@@@@@@@@@@@@@@ GOT: %s\n", );
                break;                
        }

        if(c->state==0){
            targetBright =abs(255*c->inverse-c->minBright); 
            currentRGB.b=c->targetRGB.b;
            currentRGB.g=c->targetRGB.g;
            currentRGB.r=c->targetRGB.r;
            flag_ledUpdate = checkColorAndBright(&currentRGB, &c->targetRGB, &currentBright, &targetBright, c->increment);
            setAllLed_color(led_strip_pixels, currentRGB, currentBright, c->num_of_led);
            //ESP_LOGD(TAG, "Slot:%d current RGB: %d %d %d  CurrentBright:%d", slot_num, currentRGB.r, currentRGB.g, currentRGB.b, currentBright); 
        }else{
            if (c->ledMode==MODE_DEFAULT){
                targetBright = abs(255*c->inverse-c->maxBright);  
                flag_ledUpdate = checkColorAndBright(&currentRGB, &c->targetRGB, &currentBright, &targetBright, c->increment);
                //ESP_LOGD(TAG, "DEFAULT currentBright:%f targetBright:%f increment:%f", currentBright, targetBright, increment);
                setAllLed_color(led_strip_pixels, currentRGB, currentBright, c->num_of_led);
            }else if(c->ledMode==MODE_FLASH){
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d", currentBright, targetBright); 
                if(currentBright==c->minBright){
                    targetBright=abs(255*c->inverse-c->maxBright);
                    //ESP_LOGD(TAG, "Flash min bright:%d targetBright:%d", currentBright, targetBright); 
                }else if(currentBright==c->maxBright){
                    targetBright=abs(255*c->inverse-c->minBright);
                    //ESP_LOGD(TAG, "Flash max bright:%d targetBright:%d", currentBright, targetBright); 
                }
                flag_ledUpdate = checkColorAndBright(&currentRGB, &c->targetRGB, &currentBright, &targetBright, c->increment);
                setAllLed_color(led_strip_pixels, currentRGB, currentBright, c->num_of_led);
            }else if(c->ledMode==MODE_RAINBOW){
                
                targetBright = c->maxBright;
                HsvColor hsv=RgbToHsv(c->targetRGB);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d H:%d S:%d V:%d",currentBright, targetBright, hsv.h, hsv.s, hsv.v);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d R:%d G:%d B:%d", currentBright, targetBright, currentRGB.r, currentRGB.g, currentRGB.b);
                //ESP_LOGD(TAG, "hsv before:%d %d %d", hsv.h, hsv.s, hsv.v);
                hsv.h+=c->increment;
                //ESP_LOGD(TAG, "hsv after:%d %d %d", hsv.h, hsv.s, hsv.v);
                c->targetRGB = HsvToRgb(hsv);
                //ESP_LOGD(TAG, "Flash currentBright:%d targetBright:%d R:%d G:%d B:%d", currentBright, targetBright, targetRGB.r, targetRGB.g, targetRGB.b);
                flag_ledUpdate = checkColorAndBright(&currentRGB, &c->targetRGB, &currentBright, &targetBright, c->increment);
                setAllLed_color(led_strip_pixels, currentRGB, currentBright, c->num_of_led);
            }else if(c->ledMode==MODE_RUN){
                RgbColor tmp;
                tmp.r=led_strip_pixels[0];
                tmp.g=led_strip_pixels[1];
                tmp.b=led_strip_pixels[2];
                //ESP_LOGD(TAG, "Run pixel:%d %d %d", tmp.r, tmp.g, tmp.b);
                for(int i=0; i<c->num_of_led-1; i++){
                    led_strip_pixels[i*3]=led_strip_pixels[((i+1)*3)];
                    led_strip_pixels[i*3+1]=led_strip_pixels[((i+1)*3)+1];
                    led_strip_pixels[i*3+2]=led_strip_pixels[((i+1)*3)+2];
                }
                led_strip_pixels[(c->num_of_led-1)*3]=tmp.r;
                led_strip_pixels[(c->num_of_led-1)*3+1]=tmp.g;
                led_strip_pixels[(c->num_of_led-1)*3+2]=tmp.b;

                flag_ledUpdate = 1;
            }
        }

// FIXME
//        if(forcedUpdatePeriod!=0){
//            if(xTaskGetTickCount()-lastUpdateTime>=forcedUpdatePeriod){
//                flag_ledUpdate = true;
//            }
//        }

        if(flag_ledUpdate){
            flag_ledUpdate = false;
            //ESP_LOGD(TAG, "sizeof(led_strip_pixels):%d", sizeof(led_strip_pixels));

            // printf("[ ");
            // for (int i = 0; i < sizeof(led_strip_pixels); i++)
            // {
            //     printf("%.2x ", (int)led_strip_pixels[i]);
            // }

            // printf("]\n");
            
            rmt_createAndSend(&rmt_slot_heap, led_strip_pixels, sizeof(led_strip_pixels),  slot_num);
            lastUpdateTime = xTaskGetTickCount();
        }

        //uint16_t delay = refreshPeriod - pdTICKS_TO_MS(xTaskGetTickCount()-startTick);
        //ESP_LOGD(TAG, "Led delay :%d state:%d, currentBright:%d", delay, state, currentBright); 
        //vTaskDelay(pdMS_TO_TICKS(delay));
        //vTaskDelayUntil(&lastWakeTime, refreshPeriod);

        if (xTaskDelayUntil(&lastWakeTime, pdMS_TO_TICKS(c->refreshPeriod)) == pdFALSE) {
            ESP_LOGE(TAG, "Delay missed! Adjusting wake time.");
            lastWakeTime = xTaskGetTickCount(); // Сброс времени пробуждения
        }
    }
    //EXIT:
    //vTaskDelete(NULL);
}

void start_smartLed_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();
    char tmpString[strlen("smartLed_task_")+4];
	sprintf(tmpString, "smartLed_task_%d", slot_num);
    xTaskCreatePinnedToCore(smartLed_task, tmpString, 1024*8, &slot_num,configMAX_PRIORITIES-12, NULL,1);
	ESP_LOGD(TAG,"smartLed_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}



//---------------------SWIPER LED-----------------------------
typedef struct {
	uint16_t num_led;

	uint16_t frontLen;
	uint16_t diameter;
	uint16_t *ledsCoordinate;
	uint16_t effectBufLen;
	uint8_t *effectBuf;
	double ledAngleRadian;

    uint8_t *pixelBuffer;
    
    float maxBright, minBright;

	uint16_t tick;
	uint16_t tickLenght;

	uint8_t effect;

    uint8_t *ledBrightMass;
	uint16_t brightMassLen;

	uint8_t state;

	RgbColor RGB;
	HsvColor HSV;
} swiperLed_HandleTypeDef;

float calcSin(swiperLed_HandleTypeDef *swiperLed, int tick, int lenght) {
	float phase = tick * M_PI / lenght;
	float delta = swiperLed->maxBright - swiperLed->minBright;
	//int value = sin(3.14 - phase) * delta / 2 + delta / 2;
	float value = (float)sin(phase) * delta;
	value = value + swiperLed->minBright;
	//ESP_LOGD(TAG, "tick:%d phase:%f value:%d sin:%f delta:%d", tick, phase, value, sin(phase), delta);
    if (value > swiperLed->maxBright)
		value = swiperLed->maxBright;
	return value;
}

void setMinBright(swiperLed_HandleTypeDef *swiperLed) {
	swiperLed->HSV = RgbToHsv(swiperLed->RGB);
	swiperLed->HSV.v = 255*swiperLed->minBright;
	RgbColor tmpRGB = HsvToRgb(swiperLed->HSV);
	for (int i = 0; i < swiperLed->num_led; i++) {
        //led_strip_set_pixel(swiperLed->pixelBuffer, i, swiperLed->RGB.r, swiperLed->RGB.g, swiperLed->RGB.b);
        led_strip_set_pixel(swiperLed->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
		//PwmLed_setPixel_gammaCorrection(swiperLed->pwmLed, swiperLed->RGB.r, swiperLed->RGB.g, swiperLed->RGB.b, i);
	}
    //ESP_LOGD(TAG, "setMinBright RGB:%d,%d,%d", swiperLed->RGB.r, swiperLed->RGB.g, swiperLed->RGB.b);
}

void setLedEffect(swiperLed_HandleTypeDef *swiperLed, uint8_t effect, uint16_t lenght) {
	ESP_LOGI(TAG, "start effect %d", effect);
    
    if ((effect == FADE_UP) || (effect == FADE_DOWN)||(effect == FLUSH)) {
		swiperLed->state = LED_RUN;
		swiperLed->tick = 0;
		swiperLed->effect = effect;
		swiperLed->tickLenght = lenght;
		swiperLed->HSV = RgbToHsv(swiperLed->RGB);
	}

	if ((effect == SWIPE_DOWN) || (effect == SWIPE_UP) || (effect == SWIPE_LEFT) || (effect == SWIPE_RIGHT)) {
		swiperLed->state = LED_RUN;
		swiperLed->effect = effect;
		swiperLed->tick = 0;
		swiperLed->tickLenght = lenght;
		swiperLed->HSV = RgbToHsv(swiperLed->RGB);
		swiperLed->frontLen = lenght / 2;
		swiperLed->diameter = lenght - swiperLed->frontLen;
		swiperLed->effectBufLen = lenght + swiperLed->frontLen;

		//free(swiperLed->effectBuf);
		swiperLed->effectBuf = (uint8_t*) malloc(swiperLed->effectBufLen * sizeof(uint8_t));
		memset(swiperLed->effectBuf, 255*swiperLed->minBright, swiperLed->effectBufLen);

		//printf("Front:");
		for (int i = 0; i < swiperLed->frontLen; i++) {
			swiperLed->effectBuf[i] = 255*calcSin(swiperLed, i, swiperLed->frontLen);
			//printf("%d-", swiperLed->effectBuf[i]);
		}
		//printf("\r\n");

		//calc first quarter
		uint16_t quarterNum = swiperLed->num_led / 4;
		for (int t = 0; t < quarterNum; t++) {
			swiperLed->ledsCoordinate[t] = swiperLed->frontLen + (swiperLed->diameter / 2 - swiperLed->diameter / 2 * cos(swiperLed->ledAngleRadian / 2 + swiperLed->ledAngleRadian * t));
		}
		//mirror to second quarter
		for (int t = 0; t < quarterNum; t++) {
			swiperLed->ledsCoordinate[quarterNum + t] = swiperLed->frontLen + (swiperLed->diameter / 2)+ (swiperLed->frontLen + swiperLed->diameter / 2 - swiperLed->ledsCoordinate[quarterNum - t - 1]);
		}
		//printf("ledsCoordinate:");
		for (int t = 0; t < swiperLed->num_led / 2; t++) {
			//printf("%d-", swiperLed->ledsCoordinate[t]);
		}
		//printf("\r\n");
	}
    //ESP_LOGI(TAG, "start effect end");
}

void processLedEffect(swiperLed_HandleTypeDef *swiperLed) {
	swiperLed->tick++;

    //ESP_LOGI(TAG, "procces effect tick:%d", swiperLed->tick);

	if (swiperLed->tick >= swiperLed->tickLenght) {
		swiperLed->state = LED_STOP;
		swiperLed->effect = WAITING;
		setMinBright(swiperLed);
		//return LED_STOP;
	}

	if (swiperLed->effect == FLUSH) {
		float progres = (float) swiperLed->tick / swiperLed->tickLenght;
		if (progres < 0.5) {
			swiperLed->HSV.v = 255*(swiperLed->minBright + 2 * progres * (swiperLed->maxBright - swiperLed->minBright));
		} else {
			swiperLed->HSV.v = 255*(swiperLed->maxBright - 2 * (progres - 0.5) * (swiperLed->maxBright - swiperLed->minBright));
		}
		RgbColor tmpRGB = HsvToRgb(swiperLed->HSV);

        for (int i = 0; i < swiperLed->num_led; i++) {
            led_strip_set_pixel(swiperLed->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
		// 	PwmLed_setPixel_gammaCorrection(swiperLed->pwmLed, tmpRGB.r, tmpRGB.g, tmpRGB.b, i);
		}
		// PwmLed_light(swiperLed.pwmLed);
	}

	if ((swiperLed->effect == FADE_UP) || (swiperLed->effect == FADE_DOWN)) {

		float progres = (float) swiperLed->tick / swiperLed->tickLenght;
		if (swiperLed->effect == FADE_UP) {
			swiperLed->HSV.v = 255*(swiperLed->minBright + progres * (swiperLed->maxBright - swiperLed->minBright));
		} else if (swiperLed->effect == FADE_DOWN) {
			swiperLed->HSV.v = 255*(swiperLed->maxBright - (progres * (swiperLed->maxBright - swiperLed->minBright)));
		}
		RgbColor tmpRGB = HsvToRgb(swiperLed->HSV);
		for (int i = 0; i < swiperLed->num_led; i++) {
            led_strip_set_pixel(swiperLed->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
		// 	PwmLed_setPixel_gammaCorrection(swiperLed->pwmLed, tmpRGB.r, tmpRGB.g, tmpRGB.b, i);
		}
		// PwmLed_light(swiperLed.pwmLed);
	}

	if ((swiperLed->effect == SWIPE_DOWN) || (swiperLed->effect == SWIPE_UP) || (swiperLed->effect == SWIPE_LEFT) || (swiperLed->effect == SWIPE_RIGHT)) {

		int tmp = swiperLed->effectBuf[swiperLed->effectBufLen - 1];
		for (int i = 0; i < swiperLed->effectBufLen - 1; i++) {
			swiperLed->effectBuf[swiperLed->effectBufLen - i - 1] = swiperLed->effectBuf[swiperLed->effectBufLen - i - 2];
		}
		swiperLed->effectBuf[0] = tmp;

		/*
		 printf("effectBuf:");
		 for (int i = 0; i < swiperLed->effectBufLen; i++) {
		    printf("%d-", swiperLed->effectBuf[i]);
		 }
		 printf("\r\n");
		 */

		for (int i = 0; i < swiperLed->num_led / 2; i++) {
			swiperLed->ledBrightMass[i] = swiperLed->effectBuf[swiperLed->ledsCoordinate[i]];
			swiperLed->ledBrightMass[swiperLed->num_led - 1 - i] = swiperLed->effectBuf[swiperLed->ledsCoordinate[i]];
		}

		int rotateNum= 0;
		if (swiperLed->effect == SWIPE_DOWN) {
			rotateNum = 0;
		} else if (swiperLed->effect == SWIPE_LEFT) {
			rotateNum = swiperLed->num_led / 4;
		} else if (swiperLed->effect == SWIPE_UP) {
			rotateNum = swiperLed->num_led / 2;
		} else if (swiperLed->effect == SWIPE_RIGHT) {
			rotateNum = swiperLed->num_led * 3 / 4;
		}

		for (int y = 0; y < rotateNum; y++) {
			uint8_t tmp = swiperLed->ledBrightMass[swiperLed->num_led - 1];
			for (int i = 1; i < swiperLed->num_led + 1; i++) {
				swiperLed->ledBrightMass[swiperLed->num_led - i] = swiperLed->ledBrightMass[swiperLed->num_led - 1 - i];
			}
			swiperLed->ledBrightMass[0] = tmp;
		}

		//printf("Tick:%d LedBrigtMass-", swiperLed->tick);
		for (int i = 0; i < swiperLed->num_led; i++) {
			swiperLed->HSV.v = swiperLed->ledBrightMass[i];
			RgbColor tmpRGB = HsvToRgb(swiperLed->HSV);
			//printf("%d-", swiperLed->ledBrightMass[i]);
			//todo
            led_strip_set_pixel(swiperLed->pixelBuffer, i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
            //printf("led:%d r:%d g:%d b:%d", i, tmpRGB.r, tmpRGB.g, tmpRGB.b);
            // PwmLed_setPixel_gammaCorrection(swiperLed->pwmLed, tmpRGB.r, tmpRGB.g, tmpRGB.b, i);
		}
		//printf("\r\n");

		///PwmLed_light(swiperLed.pwmLed);

	}

	//return swiperLed.state;
}
void configure_button_swiperLed(PSMARTLEDCONFIG c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* Количенство светодиодов
    */
    c->num_of_led = get_option_int_val(slot_num, "numOfLed", "", 16, 1, 1024);
    ESP_LOGD(TAG, "Set num_of_led:%d for slot:%d",c->num_of_led, slot_num);

    /* Максимальное значение яркости
    */
    c->maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 4095);
    if(c->maxBright>255)c->maxBright=255;
    if(c->maxBright<0)c->maxBright=0;
    ESP_LOGD(TAG, "Set maxBright:%d for slot:%d", c->maxBright, slot_num);

    /* Минимальное значение яркости
    */
    c->minBright = get_option_int_val(slot_num, "minBright", "", 0, 0, 4095);
    if(c->minBright<0)c->minBright=0;
    if(c->minBright>255)c->minBright=255;
    ESP_LOGD(TAG, "Set minBright:%d for slot:%d", c->minBright, slot_num);

    /* Период обновления 
    */
    c->refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 25, 1, 4096));
    ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d", c->refreshPeriod, slot_num);

    /* Начальный цвет
    */
    if (get_option_color_val(&c->targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }
    ESP_LOGD(TAG, "Set color:%d %d %d for slot:%d", c->targetRGB.r, c->targetRGB.g, c->targetRGB.b, slot_num);

    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
    	custom_topic = get_option_string_val(slot_num, "ledTopic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "actionTopic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/swiperLed_0")+3];
		sprintf(t_str, "%s/swiperLed_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    /* Анимация эфекта
    */
    stdcommand_register_enum(&c->cmds, MYCMD_swipe, "swipe", "up", "down", "left", "right");
}
void swiperLed_task(void *arg)
{
    PSMARTLEDCONFIG c = calloc(1, sizeof(SMARTLEDCONFIG));
    STDCOMMAND_PARAMS       params = { 0 };
   
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];

	me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    if(rmt_semaphore==NULL){
        rmt_semaphore = xSemaphoreCreateCounting(2, 2);
    }
    
    configure_button_swiperLed(c, slot_num);

    RgbColor targetRGB={
        .r=0,
        .g=0,
        .b=250
    };

    uint8_t led_strip_pixels[c->num_of_led * 3];

    rmt_led_heap_t rmt_slot_heap = RMT_LED_HEAP_DEFAULT();
    rmt_slot_heap.tx_chan_config.gpio_num = pin_num;
    //rmt_slot_heap.tx_chan_config.flags.io_od_mode = true;
    rmt_new_led_strip_encoder(&rmt_slot_heap.encoder_config, &rmt_slot_heap.led_encoder);
  

    swiperLed_HandleTypeDef swiperLed;
    swiperLed.num_led = c->num_of_led;
    swiperLed.pixelBuffer = led_strip_pixels;
    swiperLed.maxBright = c->maxBright;
    swiperLed.minBright = c->minBright;
    swiperLed.RGB.r = targetRGB.r;
    swiperLed.RGB.g = targetRGB.g;
    swiperLed.RGB.b = targetRGB.b;
    swiperLed.HSV= RgbToHsv((RgbColor)targetRGB);

    swiperLed.ledBrightMass = (uint8_t*) malloc(swiperLed.num_led * sizeof(uint8_t));
	swiperLed.ledAngleRadian = 2 * M_PI / swiperLed.num_led;
	swiperLed.ledsCoordinate = (uint16_t*) malloc(swiperLed.num_led / 2 * sizeof(uint16_t));
	swiperLed.state = LED_STOP;

    setMinBright(&swiperLed);

    ESP_LOGD(TAG, "swiper task config end. Slot_num:%d", slot_num);


    #define LED_EFFECT_LENGTH 30
    TickType_t lastWakeTime = xTaskGetTickCount(); 

    waitForWorkPermit(slot_num);

    while (1) {

        switch (stdcommand_receive(&c->cmds, &params, 0))
        {
            case -1: // none
                break;

            case MYCMD_swipe:
                switch (params.enumResult)
                {
                    case 0:// up
                        setLedEffect(&swiperLed, SWIPE_UP, LED_EFFECT_LENGTH);
                        break;
                    case 1:// down
                        setLedEffect(&swiperLed, SWIPE_DOWN, LED_EFFECT_LENGTH);
                        break;
                    case 2:// left
                        setLedEffect(&swiperLed, SWIPE_LEFT, LED_EFFECT_LENGTH);
                        break;
                    default:// right or so
                        setLedEffect(&swiperLed, SWIPE_RIGHT, LED_EFFECT_LENGTH);
                        break;
                }
                break;

            default:
                break;                
        }
    
        if(swiperLed.state == LED_RUN){
            processLedEffect(&swiperLed);
            rmt_createAndSend(&rmt_slot_heap, led_strip_pixels, sizeof(led_strip_pixels),  slot_num);
        }
          
        
        vTaskDelayUntil(&lastWakeTime, c->refreshPeriod);
        //vTaskDelay(pdMS_TO_TICKS(refreshPeriod));
    }
}

void start_swiperLed_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreatePinnedToCore(swiperLed_task, "swiperLed_task", 1024*4, &slot_num,12, NULL,1);
	ESP_LOGD(TAG,"swiperLed_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}

//---------------------LED RING-----------------------------
void ledUpdate(uint8_t *currentMass, uint8_t *targetMass, uint16_t size, uint8_t increment, rmt_led_heap_t *rmt_heap, uint8_t slot_num) {
    //ESP_LOGD(TAG, "ledUpdate: slot_num:%d", slot_num);
    uint16_t sum = 0;
    for(int i=0; i<size; i++){
        if(currentMass[i] != targetMass[i]){
            //ESP_LOGD(TAG, "ledUpdate: slot_num:%d i:%d currentMass:%d targetMass:%d", slot_num, i, currentMass[i], targetMass[i]);
            sum ++;
            if(currentMass[i] < targetMass[i]){
                int16_t temp = currentMass[i]+ increment;
                if(temp >= targetMass[i]){
                    currentMass[i] = targetMass[i];
                }else{
                    currentMass[i] += increment;
                }
            }else{
                int16_t temp = currentMass[i] - increment;
                if(temp <= targetMass[i]){
                    currentMass[i] = targetMass[i];
                }else{
                    currentMass[i] -= increment;
                }
            }
        }
    }
    if(sum != 0){
        // for(int i=0; i<size/3; i++){
        //     printf(" %d", currentMass[i*3+2]);
        // }
        // printf("\n");
        //ESP_LOGD(TAG, "ledUpdate: slot_num:%d sum:%d", slot_num, sum);
        rmt_createAndSend(rmt_heap, currentMass, size, slot_num);
    }
}

void calcRingBrightness(uint8_t *pixelMass, RgbColor color, uint16_t num_of_led, float currentPos, uint16_t effectLen, uint16_t numOfPos, int16_t maxBright, int16_t minBright) {
    // Convert position to LED scale
    //ESP_LOGD(TAG, "calcRingBrightness: currentPos:%f effectLen:%d numOfPos:%d num_of_led:%d minBright:%d maxBright:%d", currentPos, effectLen, numOfPos, num_of_led, minBright, maxBright);
    
    float positionScale = (float)num_of_led / numOfPos;
    float centrPos = (currentPos * positionScale);
    
    float fMaxB = (float)maxBright/255;
    float fMinB = (float)minBright/255;

    // Calculate half effect length
    uint16_t halfEffect = effectLen/2;
    
    for(int i = 0; i < num_of_led; i++) {
        // Calculate shortest distance considering ring topology
        float distance = i - centrPos;
        if(distance > num_of_led/2) distance -= num_of_led;
        if(distance < -num_of_led/2) distance += num_of_led;
        distance = fabs(distance);
        
        if(distance <= halfEffect) {
            // Linear interpolation between maxBright and minBright
            float ratio =(1- distance / halfEffect) * (fMaxB - fMinB) + fMinB;
            //printf(" %f", ratio);
            pixelMass[i*3] = gamma_8[(uint8_t)(color.g * ratio)];
            pixelMass[i*3+1] = gamma_8[(uint8_t)(color.r * ratio)];
            pixelMass[i*3+2] = gamma_8[(uint8_t)(color.b * ratio)];
            //ESP_LOGD(TAG, "calcRingBrightness: i:%d distance:%f ratio:%f bright:%f", i, distance, ratio, brightArray[i]);
            
        } else {
            pixelMass[i*3] = gamma_8[(uint8_t)(color.g * fMinB)];
            pixelMass[i*3+1] = gamma_8[(uint8_t)(color.r * fMinB)];
            pixelMass[i*3+2] = gamma_8[(uint8_t)(color.b * fMinB)];
            //printf(" %f", fMinB);
        }
        //printf(" %d", pixelMass[i*3+2]);
    }
    //printf("\n");
}
/*
    Модуль поддержки светового кольца
*/
void configure_button_ledRing(PSMARTLEDCONFIG c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* Количенство светодиодов
    */
    c->num_of_led = get_option_int_val(slot_num, "numOfLed", "", 24, 1, 1024);
    ESP_LOGD(TAG, "Set num_of_led:%d for slot:%d",c->num_of_led, slot_num);

    /* Величина приращения
    */
    c->increment = get_option_int_val(slot_num, "increment", "", 255, 0, 255);
    if(c->increment<1)c->increment=1;
    if(c->increment>255)c->increment=255;
    ESP_LOGD(TAG, "Set increment:%d for slot:%d", c->increment, slot_num);

    /* Максимальное значение яркости
    */
    c->maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 4095);
    if(c->maxBright>255)c->maxBright=255;
    if(c->maxBright<0)c->maxBright=0;
    ESP_LOGD(TAG, "Set maxBright:%d for slot:%d", c->maxBright, slot_num);

    /* Минимальное значение яркости
    */
    c->minBright = get_option_int_val(slot_num, "minBright", "", 0, 0, 4095);
    if(c->minBright<0)c->minBright=0;
    if(c->minBright>255)c->minBright=255;
    ESP_LOGD(TAG, "Set minBright:%d for slot:%d", c->minBright, slot_num);

    /* Период обновления 
    */
    c->refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 1000/30, 1, 4096));
    ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d", c->refreshPeriod, slot_num);

#define NUMBER_OF_LEDS  (c->num_of_led)
    /* Количество позиций
    */        
    c->numOfPos = get_option_int_val(slot_num, "numOfPos", "", NUMBER_OF_LEDS, 1, 4096);
    ESP_LOGD(TAG, "Set numOfPos:%d for slot:%d", c->numOfPos, slot_num);

#define NUMBER_OF_LEDS_DIVIDEV_BY_4  (c->num_of_led / 4)
    /* Длинна эффекта
    */
    c->effectLen = get_option_int_val(slot_num, "effectLen", "", NUMBER_OF_LEDS_DIVIDEV_BY_4, 1, 4096);
    ESP_LOGD(TAG, "Set effectLen:%d for slot:%d", c->effectLen, slot_num);

    /* Состояние по умолчанию
    */
    c->state = get_option_int_val(slot_num, "defaultState", "", 0, 0, 1);
    ESP_LOGD(TAG, "Set def_state:%d for slot:%d", c->state, slot_num);
      
    /* Инверсия направления эффекта
    */
    c->dir = c->inverse = get_option_flag_val(slot_num, "dirInverse") ? 1 : -1;
    ESP_LOGD(TAG, "Set dir inverse %d for slot:%d", c->dir, slot_num);

    /* Смещение эффекта
    */
    c->offset = get_option_int_val(slot_num, "offset", "", 0, 0, NUMBER_OF_LEDS);
    ESP_LOGD(TAG, "Set offset:%d for slot:%d", c->offset, slot_num);

    /* Задаёт режим анимации */
    if ((c->ledMode = get_option_enum_val(slot_num, "ledMode", "default", "flash", "glitch", "swiper", "rainbow", "run", NULL)) < 0)
    {
        ESP_LOGE(TAG, "ledMode: unricognized value");
    }
    ESP_LOGD(TAG, "Set ledMode:%d for slot:%d", c->ledMode, slot_num);

    /* Начальный цвет
    */
    if (get_option_color_val(&c->targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }
    ESP_LOGD(TAG, "Set color:%d %d %d for slot:%d", c->targetRGB.r, c->targetRGB.g, c->targetRGB.b, slot_num);

    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
        /* Определяет топик для MQTT сообщений */
    	custom_topic = get_option_string_val(slot_num, "ledTopic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/ledRing_0")+3];
		sprintf(t_str, "%s/ledRing_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    /* Числовое значение.
       задаёт текущее состояние светодиода (вкл/выкл)
    */
    stdcommand_register(&c->cmds, MYCMD_default, NULL, PARAMT_int);

    /* Установить новый целевой цвет. 
       Цвет задаётся десятичными значениями R G B через пробел
    */
    stdcommand_register(&c->cmds, MYCMD_setRGB, "setRGB", PARAMT_int, PARAMT_int, PARAMT_int);

    /* Установить новую текущую позичию
    */
    stdcommand_register(&c->cmds, MYCMD_setPos, "setPos", PARAMT_int);

}
void ledRing_task(void *arg){
    
    PSMARTLEDCONFIG c = calloc(1, sizeof(SMARTLEDCONFIG));
    uint8_t prevState=255;
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];
    STDCOMMAND_PARAMS       params = { 0 };

	me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    if(rmt_semaphore==NULL){
        rmt_semaphore = xSemaphoreCreateCounting(2, 2);
    }

    configure_button_ledRing(c, slot_num);

    uint8_t current_pixels_mass[c->num_of_led * 3];
    memset(current_pixels_mass, 0, c->num_of_led * 3);
    uint8_t target_pixels_mass[c->num_of_led * 3];
    memset(target_pixels_mass, 0, c->num_of_led * 3);

    rmt_led_heap_t rmt_slot_heap = RMT_LED_HEAP_DEFAULT();
    rmt_slot_heap.tx_chan_config.gpio_num = pin_num;
    //rmt_slot_heap.tx_chan_config.flags.io_od_mode = true;
    rmt_new_led_strip_encoder(&rmt_slot_heap.encoder_config, &rmt_slot_heap.led_encoder);
 
    // float currentBrightness[num_of_led];
    // for(int i=0;i<num_of_led;i++){
    //     currentBrightness[i]= (state==0) ? minBright: maxBright;
    // }

    float currentPos=c->numOfPos-1+c->offset;
    float targetPos=c->offset;

    float fIncrement = (float)c->increment/255;
    
    //vTaskDelay(pdMS_TO_TICKS(1000));
    TickType_t lastWakeTime = xTaskGetTickCount(); 

    waitForWorkPermit(slot_num);

    while(1){
        // command_message_t temp_msg;
        // command_message_t msg;
        // uint8_t recv_state=0;

        switch (stdcommand_receive(&c->cmds, &params, 0))
        {
            case -1: // none
                break;

            case MYCMD_default:
                c->state = params.p[0].i;
                ESP_LOGD(TAG, "Change state to:%d", c->state);
                break;

            case MYCMD_setRGB:
                c->targetRGB.r = params.p[0].i;
                c->targetRGB.g = params.p[1].i;
                c->targetRGB.b = params.p[2].i;

                ESP_LOGD(TAG, "Slot:%d target RGB: %d %d %d", slot_num, c->targetRGB.r, c->targetRGB.g, c->targetRGB.b); 
                break;

            case MYCMD_setPos:
                if(c->dir==1){
                    targetPos = params.p[0].i+c->offset;
                }else{
                    targetPos = c->numOfPos-1-params.p[0].i+c->offset;
                }
                if(targetPos<0){
                    targetPos=targetPos+(c->numOfPos);
                }else if(targetPos>c->numOfPos-1){
                    targetPos=targetPos-(c->numOfPos);
                }
                break;


            default:
                //ESP_LOGD(TAG, "@@@@@@@@@@@@@@@@@@ GOT: %s\n", );
                //printf("@@@@@@@@@@@@@@@@@@ GOT!!!!\n");
                break;                
        }

        if(c->state==1){
            if(c->state!=prevState){
                prevState=c->state;
                //ESP_LOGD(TAG, "Set state to 0");
                //float fMinB = (float)minBright/255;
                float fMaxB = (float)c->maxBright/255;
                for(int i = 0; i < c->num_of_led; i++) {
                    target_pixels_mass[i*3] = gamma_8[(uint8_t)(c->targetRGB.g * fMaxB)];
                    target_pixels_mass[i*3+1] = gamma_8[(uint8_t)(c->targetRGB.r * fMaxB)];
                    target_pixels_mass[i*3+2] = gamma_8[(uint8_t)(c->targetRGB.b * fMaxB)]; 
                }
            }   
        }else{
            if(c->state!=prevState){
                prevState=c->state;
                currentPos = targetPos-fIncrement;
            }

            if(c->ledMode==MODE_RUN){
                targetPos += fIncrement*c->dir;
                if(targetPos<0){
                    targetPos=targetPos+(c->numOfPos);
                }else if(targetPos>c->numOfPos-1){
                    targetPos=targetPos-(c->numOfPos);
                }
            }
            
            if(currentPos!=targetPos){
                //ESP_LOGD(TAG, "currentPos:%f targetPos:%f increment:%d fIn:%f", currentPos, targetPos, increment, fIncrement);
                if(fabs(currentPos-targetPos)<fIncrement){
                    currentPos = targetPos;
                }else{
                    if(fabs(currentPos-targetPos)<c->numOfPos/2){
                        currentPos = (currentPos>targetPos) ? currentPos-fIncrement : currentPos+fIncrement;
                    }else{
                        currentPos = (currentPos>targetPos) ? currentPos+fIncrement : currentPos-fIncrement;
                    }
                    if(currentPos<0){
                        currentPos=currentPos+(c->numOfPos);
                    }else if(currentPos>c->numOfPos-1){
                        currentPos=currentPos-(c->numOfPos);
                    }
                }
                calcRingBrightness(&target_pixels_mass[0], c->targetRGB, c->num_of_led, currentPos, c->effectLen, c->numOfPos, c->maxBright, c->minBright);
            }
        }
        ledUpdate(&current_pixels_mass[0], &target_pixels_mass[0], c->num_of_led*3, c->increment, &rmt_slot_heap, slot_num);
        vTaskDelayUntil(&lastWakeTime, c->refreshPeriod);
    }
}

void start_ledRing_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreatePinnedToCore(ledRing_task, "ledRing_task", 1024*4, &slot_num,12, NULL,1);
	ESP_LOGD(TAG,"ledRing_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}



//---------------------LED BAR-----------------------------

uint8_t colorChek(uint8_t currentColor, uint8_t targetColor, uint8_t increment){
    if(currentColor<targetColor){
        if(targetColor-currentColor>increment){
            return currentColor+increment;
        }else{
            return targetColor;
        }
    }else if(currentColor>targetColor){
        if(currentColor-targetColor>increment){
            return currentColor-increment;
        }else{
            return targetColor;
        }
    }else{
        return targetColor;
    }
}
/*
    Модуль поддержки светового блока
*/
void configure_button_ledBar(PSMARTLEDCONFIG c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

    /* Количенство светодиодов
    */
    c->num_of_led = get_option_int_val(slot_num, "numOfLed", "", 24, 1, 1024);
    ESP_LOGD(TAG, "Set num_of_led:%d for slot:%d",c->num_of_led, slot_num);

    /* Величина приращения
    */
    c->increment = get_option_int_val(slot_num, "increment", "", 255, 0, 255);
    if(c->increment<1)c->increment=1;
    if(c->increment>255)c->increment=255;
    ESP_LOGD(TAG, "Set increment:%d for slot:%d", c->increment, slot_num);

    /* Максимальное значение яркости
    */
    c->maxBright = get_option_int_val(slot_num, "maxBright", "", 255, 0, 4095);
    if(c->maxBright>255)c->maxBright=255;
    if(c->maxBright<0)c->maxBright=0;
    ESP_LOGD(TAG, "Set maxBright:%d for slot:%d", c->maxBright, slot_num);


    /* Минимальное значение яркости
    */
    c->minBright = get_option_int_val(slot_num, "minBright", "", 0, 0, 4095);
    if(c->minBright<0)c->minBright=0;
    if(c->minBright>255)c->minBright=255;
    ESP_LOGD(TAG, "Set minBright:%d for slot:%d", c->minBright, slot_num);

    /* Период обновления 
    */
    c->refreshPeriod = 1000/(get_option_int_val(slot_num, "refreshRate", "", 1000/30, 1, 4096));
    ESP_LOGD(TAG, "Set refreshPeriod:%d for slot:%d", c->refreshPeriod, slot_num);
    	
#define NUMBER_OF_LEDS  (c->num_of_led)
    /* Количество позиций
    */        
    c->numOfPos = get_option_int_val(slot_num, "numOfPos", "", NUMBER_OF_LEDS, 1, 4096);
    ESP_LOGD(TAG, "Set numOfPos:%d for slot:%d", c->numOfPos, slot_num);

    /* Состояние по умолчанию
    */
    c->state = get_option_int_val(slot_num, "defaultState", "", 0, 0, 1);
    ESP_LOGD(TAG, "Set def_state:%d for slot:%d", c->state, slot_num);

    /* Инверсия направления эффекта
    */
    c->dir = c->inverse = get_option_flag_val(slot_num, "dirInverse") ? 1 : -1;
    ESP_LOGD(TAG, "Set dir inverse %d for slot:%d", c->dir, slot_num);

    /* Смещение эффекта
    */
    c->offset = get_option_int_val(slot_num, "offset", "", 0, 0, NUMBER_OF_LEDS);
    ESP_LOGD(TAG, "Set offset:%d for slot:%d", c->offset, slot_num);

    /* Начальный цвет
    */
    if (get_option_color_val(&c->targetRGB, slot_num, "RGBcolor", "0 0 255") != ESP_OK)
    {
        ESP_LOGE(TAG, "Wrong color value slot:%d", slot_num);
    }
    ESP_LOGD(TAG, "Set color:%d %d %d for slot:%d", c->targetRGB.r, c->targetRGB.g, c->targetRGB.b, slot_num);

/// -----------------------------------

    if (strstr(me_config.slot_options[slot_num], "ledTopic") != NULL) {
		char* custom_topic=NULL;
        /* Определяет топик для MQTT сообщений */
    	custom_topic = get_option_string_val(slot_num, "ledTopic");
		me_state.action_topic_list[slot_num]=strdup(custom_topic);
		ESP_LOGD(TAG, "action_topic:%s", me_state.action_topic_list[slot_num]);
    }else{
		char t_str[strlen(me_config.deviceName)+strlen("/ledBar_0")+3];
		sprintf(t_str, "%s/ledBar_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=strdup(t_str);
		ESP_LOGD(TAG, "Standart action_topic:%s", me_state.action_topic_list[slot_num]);
	} 

    /* Установить новый целевой цвет. 
       Цвет задаётся десятичными значениями R G B через пробел
    */
    stdcommand_register(&c->cmds, MYCMD_setRGB, "setRGB", PARAMT_int, PARAMT_int, PARAMT_int);

    /* Установить новую текущую позичию
    */
    stdcommand_register(&c->cmds, MYCMD_setPos, "setPos", PARAMT_int);
}
void ledBar_task(void *arg)
{
    PSMARTLEDCONFIG c = calloc(1, sizeof(SMARTLEDCONFIG));
    STDCOMMAND_PARAMS       params = { 0 };
    
	int slot_num = *(int*) arg;
	uint8_t pin_num = SLOTS_PIN_MAP[slot_num][1];

	me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

    if(rmt_semaphore==NULL){
        rmt_semaphore = xSemaphoreCreateCounting(2, 2);
    }

    //uint8_t prevState=255;

    configure_button_ledBar(c, slot_num);

    RgbColor currentRGB={
        .r=0,
        .g=0,
        .b=0
    };

    uint8_t current_bright_mass[c->num_of_led];
    memset(current_bright_mass, 0, c->num_of_led);
    uint8_t target_bright_mass[c->num_of_led];
    memset(target_bright_mass, 0, c->num_of_led);

    uint8_t led_strip_pixels[c->num_of_led * 3];

    rmt_led_heap_t rmt_slot_heap = RMT_LED_HEAP_DEFAULT();
    rmt_slot_heap.tx_chan_config.gpio_num = pin_num;
    //rmt_slot_heap.tx_chan_config.flags.io_od_mode = true;
    rmt_new_led_strip_encoder(&rmt_slot_heap.encoder_config, &rmt_slot_heap.led_encoder);
  
    // float currentBrightness[num_of_led];
    // for(int i=0;i<num_of_led;i++){
    //     currentBrightness[i]= (state==0) ? minBright: maxBright;
    // }
    float ledToPosRatio = (float)c->num_of_led/c->numOfPos;

    float currentPos=-1;
    int targetPos=0;

    uint8_t flag_ledUpdate=1;
    // float fIncrement = (float)increment/255;
    
    //vTaskDelay(pdMS_TO_TICKS(1000));
    TickType_t lastWakeTime = xTaskGetTickCount(); 

    waitForWorkPermit(slot_num);
    while(1){
        switch (stdcommand_receive(&c->cmds, &params, 0))
        {
            case -1: // none
                break;

            case 0: // Unknown or absent keyword
                if ((params.count == 1) && (params.p[0].type == PARAMT_int))
                {
                    c->state = params.p[0].i;
                    ESP_LOGD(TAG, "Change state to:%d", c->state);
                }
                break;

            case MYCMD_setRGB:
                c->targetRGB.r = params.p[0].i;
                c->targetRGB.g = params.p[1].i;
                c->targetRGB.b = params.p[2].i;

                ESP_LOGD(TAG, "Slot:%d target RGB: %d %d %d", slot_num, c->targetRGB.r, c->targetRGB.g, c->targetRGB.b); 
                break;

            case MYCMD_setPos:
                targetPos += params.p[0].i;
                if(targetPos>c->numOfPos) targetPos=c->numOfPos;
                ESP_LOGD(TAG, "Change pos to:%d", targetPos);
                break;

            default:
                break;                
        }

        if(targetPos!=currentPos){
            currentPos=targetPos;
            float ledPos = (ledToPosRatio * currentPos);
            for(int i=0;i<c->num_of_led;i++){
                int curentLedPos = i + c->offset;
                if(curentLedPos>c->num_of_led-1){
                    curentLedPos = curentLedPos - c->num_of_led;
                }
                if((i == (int)ledPos)&&(i>0)){
                    float ratio = ledPos - (int)ledPos;
                    target_bright_mass[curentLedPos]=(int)(c->maxBright*ratio);
                    if(target_bright_mass[curentLedPos]<c->minBright){
                        target_bright_mass[curentLedPos]=c->minBright;
                    }
                }else if(i>(int)ledPos-1){
                    target_bright_mass[curentLedPos]=c->minBright;
                }else{
                    target_bright_mass[curentLedPos]=c->maxBright;
                }
            }
            // printf("targetMass:");
            // for(int i=0;i<num_of_led;i++){
            //     printf(" %d", target_bright_mass[i]);
            // }
            // printf("\n");

            // printf("currentMass:");
            // for(int i=0;i<num_of_led;i++){
            //     printf(" %d", current_bright_mass[i]);
            // }
            // printf("\n");

        }

        if(memcmp(&currentRGB, &c->targetRGB, sizeof(RgbColor))){
            if(currentRGB.r!=c->targetRGB.r){
                currentRGB.r = colorChek(currentRGB.r, c->targetRGB.r, c->increment);
                flag_ledUpdate=true;
            }
            if(currentRGB.g!=c->targetRGB.g){
                currentRGB.g = colorChek(currentRGB.g, c->targetRGB.g, c->increment);
                flag_ledUpdate=true;
            }
            if(currentRGB.b!=c->targetRGB.b){
                currentRGB.b = colorChek(currentRGB.b, c->targetRGB.b, c->increment);
                flag_ledUpdate=true;
            }
        }

        for(int i=0;i<c->num_of_led;i++){
            if(target_bright_mass[i]!=current_bright_mass[i]){
                
                flag_ledUpdate=true;
                if(target_bright_mass[i]>current_bright_mass[i]){
                    if(i>0){
                        if(current_bright_mass[i-1]!=target_bright_mass[i-1]){
                            goto end_loop;
                        }
                    }
                    if(abs(target_bright_mass[i]-current_bright_mass[i])<c->increment){
                        current_bright_mass[i] = target_bright_mass[i];
                    }else{
                        current_bright_mass[i] = current_bright_mass[i] + c->increment;
                    }
                    break;
                }else{
                    if(i<c->num_of_led-1){
                        if(current_bright_mass[i+1]!=target_bright_mass[i+1]){
                            //ESP_LOGD(TAG, "skip:%d nextTarget:%d nextBright:%d", i, target_bright_mass[i+1], current_bright_mass[i+1]);
                            goto end_loop;
                        }
                    }
                    if(abs(target_bright_mass[i]-current_bright_mass[i])<c->increment){
                        current_bright_mass[i] = target_bright_mass[i];
                    }else{
                        current_bright_mass[i] = current_bright_mass[i] - c->increment;
                    }
                    //ESP_LOGD(TAG, "Changed bright i:%d target:%d current:%d", i, target_bright_mass[i], current_bright_mass[i]);
                    break;
                }
            }
            end_loop:;
        }

        if(flag_ledUpdate){
            for(int i=0;i<c->num_of_led;i++){
                int index = i;
                if(c->dir<0){
                    index = c->num_of_led - i - 1;
                }
                float  tmpBright = (float)current_bright_mass[i]/255;
                led_strip_pixels[index*3] = gamma_8[(uint8_t)(currentRGB.r * tmpBright)];
                led_strip_pixels[index*3+1] = gamma_8[(uint8_t)(currentRGB.g * tmpBright)];
                led_strip_pixels[index*3+2] = gamma_8[(uint8_t)(currentRGB.b * tmpBright)];
            }

            flag_ledUpdate = false;
            //ESP_LOGD(TAG, "sizeof(led_strip_pixels):%d", sizeof(led_strip_pixels));
            rmt_createAndSend(&rmt_slot_heap, led_strip_pixels, sizeof(led_strip_pixels),  slot_num);
        }

        vTaskDelayUntil(&lastWakeTime, c->refreshPeriod);
    }
}

void start_ledBar_task(int slot_num){
    uint32_t heapBefore = xPortGetFreeHeapSize();

    xTaskCreatePinnedToCore(ledBar_task, "ledBar_task", 1024*4, &slot_num,12, NULL,1);
	ESP_LOGD(TAG,"ledBar_task created for slot: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
const char * get_manifest_smartLed()
{
	return manifesto;
}
