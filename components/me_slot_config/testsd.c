// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include "esp_system.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "freertos/portmacro.h"
#include "stateConfig.h"
#include "me_slot_config.h"
#include "reporter.h"
#include <mbdebug.h>
#include <sd_card.h>
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "bsp/board.h"
#include "rom/gpio.h"
#include "esp_vfs_fat.h"
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "ff.h"

#include <stdcommand.h>
#include <stdreport.h>

#include <generated_files/gen_testsd.h>


// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define LOG_LOCAL_LEVEL 	ESP_LOG_DEBUG

#define CHECK_EXECUTE_RESULT(err, str) do { \
    if ((err) !=ESP_OK) { \
        ESP_LOGE(TAG, str" (0x%x).", err); \
        goto cleanup; \
    } \
    } while(0)

#define CHUNK_SECTORS		128

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_TESTSD_CONFIG
{
	bool 					used;
	int 					pattern_num;
	TaskHandle_t			s_task_handle;
	int 					slot_num;

	int 					testType;
	int 					maxErrors;
	int 					runDelay;

    STDCOMMANDS             cmds;

	int                    	stateReport;
    int                    	resultReport;
	int 					progressReport;

	int 					cardDetected;
	uint8_t 				ledPin;

	char *					custom_topic;
	int 					flag_custom_topic;

} TESTSD_CONFIG, * PTESTSD_CONFIG; 

typedef struct __tag_SDCARDPINSET
{
	const char * 			name;
	uint8_t 				clk_pin;
	uint8_t 				cmd_pin;
	uint8_t 				d0_pin;
	uint8_t 				led_pin;
} SDCARDPINSET, *PSDCARDPINSET;


typedef enum
{
	rtpCMD_start= 0,
    rtpCMD_stop,
} rtpCMD;

typedef enum
{
	STATE_stopped	= 0,
	STATE_init,
	STATE_running,
	STATE_formatting,
	STATE_done
} STATE;	

typedef enum
{
	TESTT_read,
	TESTT_readwrite,
	TESTT_readback
} TESTT;

typedef struct __tag_TESTSCORE
{
	unsigned long 			total;
	unsigned long 			current;
	unsigned long 			erroneous;

	unsigned long 			blockSize;
	unsigned long 			chunkSectors;

	STATE					state;
	int 					result;

	char * 					readBuffer;
	char * 					compareBuffer;

	TickType_t 				beginTime;
} TESTSCORE, *PTESTSCORE;




// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

static 	int 				host_inited = false;

static SemaphoreHandle_t s_sdcard_mutex = NULL;

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;
extern uint8_t led_segment;

static const char *TAG = "TESTSD";

static sdmmc_card_t *card;
static sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
static sdmmc_host_t host = SDMMC_HOST_DEFAULT();



static char * stateNames[] = 
{
	"stopped",
	"initializing",
	"running",
	"formatting",
	"done"
};

/* result: 0=none, 1=success, -1=error, -2=format_error */
static const char * resultNames[] = 
{
	"formatError",	/* -2 */
	"error",		/* -1 */
	"none",			/*  0 */
	"success"		/*  1 */
};

static char * testTypeNames[] = 
{
	"simple read", 
	"read and write", 
	"write and read back"
};



// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

const char * getStateName(STATE state)
{
	return stateNames[state];
}

static const char * getResultName(int result)
{
	/* result range: -2..1 → index 0..3 */
	int idx = result + 2;
	if (idx < 0) idx = 0;
	if (idx > 3) idx = 3;
	return resultNames[idx];
}


/* 
    Модуль тестирования SD-карт
*/
static int detect_sd_card_presence(void)
{
	if (!host_inited) {
		/* Хост не инициализирован — проверяем наличие карты по уровню на DATA0 пине.
		   SD-карта имеет внутренние pull-up на линиях; без карты pullup на плате даёт HIGH,
		   при вставленной карте линия CMD тоже HIGH. Используем sdmmc_card_init через хост. 
		   Без хоста надёжная детекция невозможна — возвращаем -1 (неизвестно). */
		return -1;
	}

	sdmmc_card_t *probe = calloc(1, sizeof(sdmmc_card_t));
	if (!probe) return 0;

	int result = (sdmmc_card_init(&host, probe) == ESP_OK) ? 1 : 0;
	free(probe);

	return result;
}

static esp_err_t format_sd_card(void)
{
	BYTE pdrv;
	FATFS *fs = NULL;
	void *work_buf = NULL;
	esp_err_t result = ESP_FAIL;

	ff_diskio_register_sdmmc(0, card);
	pdrv = 0;

	fs = calloc(1, sizeof(FATFS));
	if (!fs) {
		ESP_LOGE(TAG, "Format: FATFS alloc failed");
		goto fmt_cleanup;
	}

	FRESULT fres = f_mount(fs, "", 0);
	if (fres != FR_OK) {
		ESP_LOGE(TAG, "Format: f_mount failed (%d)", fres);
		goto fmt_cleanup;
	}

	const size_t workbuf_size = 4096;
	work_buf = malloc(workbuf_size);
	if (!work_buf) {
		ESP_LOGE(TAG, "Format: workbuf alloc failed");
		goto fmt_cleanup;
	}

	const MKFS_PARM opt = { FM_FAT32, 0, 0, 0, 0 };
	ESP_LOGI(TAG, "Formatting SD card as FAT32...");
	fres = f_mkfs("", &opt, work_buf, workbuf_size);
	if (fres != FR_OK) {
		ESP_LOGE(TAG, "Format: f_mkfs failed (%d)", fres);
		goto fmt_cleanup;
	}

	ESP_LOGI(TAG, "SD card formatted successfully");
	result = ESP_OK;

fmt_cleanup:
	f_mount(NULL, "", 0);
	ff_diskio_register(pdrv, NULL);
	if (fs) free(fs);
	if (work_buf) free(work_buf);

	return result;
}

void configure_testsd(PTESTSD_CONFIG	c, int slot_num)
{
    stdcommand_init(&c->cmds, slot_num);

	/* Светодиод индикации наличия SD-карты
	*/
	c->ledPin = SLOTS_PIN_MAP[slot_num][3];
	if (c->ledPin != 0) {
		esp_rom_gpio_pad_select_gpio(c->ledPin);
		gpio_set_direction(c->ledPin, GPIO_MODE_OUTPUT);
		gpio_set_level(c->ledPin, 1);
	}
	c->cardDetected = -1; // неизвестное начальное состояние

	/* Задаёт тип теста
	   read - только чтение
	   readwrite - чтение и запись обратно
	   readback - чтение, запись и проверка вторичным чтением
	*/
	c->testType = get_option_enum_val(slot_num, "testType", "readwrite", "read", "readback", NULL);
	if (c->testType < 0) c->testType = 0;
	/* remap: 0=readwrite, 1=read, 2=readback → enum TESTT */
	switch (c->testType) {
		case 0: c->testType = TESTT_readwrite; break;
		case 1: c->testType = TESTT_read;      break;
		case 2: c->testType = TESTT_readback;   break;
	}
	ESP_LOGD(TAG, "Slot:%d test type[%d] = %s", slot_num, c->testType, testTypeNames[c->testType]);

	/* Определяет порог ошибок для останова тестирования
	*/
	c->maxErrors = get_option_int_val(slot_num, "maxErrors", "", 1, 1, 4096);
	ESP_LOGD(TAG, "Slot:%d max errors = %d", slot_num, c->maxErrors);

	/* Задаёт период ожидания в тиках между чтением блоков
	*/
	c->runDelay = get_option_int_val(slot_num, "runDelay", "", 0, 0, 4096);
	ESP_LOGD(TAG, "Slot:%d run delay = %d", slot_num, c->runDelay);


    /* Рапортует о текущем статусе (фазе) тестирования, а также детекции карты.
       Возможные значения: "stopped", "initializing", "running", "formatting", "done", "inserted", "ejected"
	*/
	c->stateReport = stdreport_register(RPTT_string, slot_num, "string", "state");

    /* Рапортует результат завершенного тестирования.
       Возможные значения: "none", "success", "error", "formatError"
	*/
	c->resultReport = stdreport_register(RPTT_string, slot_num, "string", "result");

	/* Рапортует прогресс тестирования в целых процентах.
	   Возможные значения: 0 — 100
	*/
	c->progressReport = stdreport_register(RPTT_int, slot_num, "percent", "progress");

    /* Команда запускает тестирование
    */
    stdcommand_register(&c->cmds, rtpCMD_start, "start", PARAMT_none);

    /* Команда остатавливает тестирование
    */
    stdcommand_register(&c->cmds, rtpCMD_stop, "stop", PARAMT_none);

	if (strstr(me_config.slot_options[slot_num], "topic")!=NULL){
		/* Определяет топик для MQTT сообщений */
		c->custom_topic = get_option_string_val(slot_num,"topic", "/testsd_0");
		ESP_LOGD(TAG, "Custom topic:%s", c->custom_topic);
		c->flag_custom_topic=1;
	}

    if(c->flag_custom_topic==0){
		char *str = calloc(strlen(me_config.deviceName)+strlen("/testsd_")+4, sizeof(char));
		sprintf(str, "%s/testsd_%d",me_config.deviceName, slot_num);
		me_state.action_topic_list[slot_num]=str;
		me_state.trigger_topic_list[slot_num]=str;
	}else{
		me_state.action_topic_list[slot_num]=c->custom_topic;
		me_state.trigger_topic_list[slot_num]=c->custom_topic;
	}
}

static PSDCARDPINSET check_pinset(PSDCARDPINSET ps)
{
	PSDCARDPINSET	result = 0;

	gpio_pad_select_gpio(ps->clk_pin);
	gpio_set_direction(ps->clk_pin, GPIO_MODE_INPUT);
	
	gpio_pad_select_gpio(ps->d0_pin);
	gpio_set_direction(ps->d0_pin, GPIO_MODE_INPUT);
	
	gpio_pad_select_gpio(ps->cmd_pin);
	gpio_set_direction(ps->cmd_pin, GPIO_MODE_INPUT);

	if ( (gpio_get_level(ps->clk_pin) == 1)  	&&
	     (gpio_get_level(ps->d0_pin)  == 1) 	&&
		 (gpio_get_level(ps->cmd_pin) == 1)		)
	{
		ESP_LOGD(TAG, "Pinset '%s' seems to be right", ps->name);
		result = ps;
	}

	return result;
} 

static esp_err_t init_sdmmc_host(int slot, const void *slot_config, int *out_slot)
{
    *out_slot = slot;
    return sdmmc_host_init_slot(slot, (const sdmmc_slot_config_t*) slot_config);
}

int testsd_init() 
{
	int 			result = ESP_FAIL;
	PSDCARDPINSET	curPs;
	SDCARDPINSET	oldPs = { "Old slot 1", 47, 21, 40, 48 };
	SDCARDPINSET	newPs = { "New slot 1", 41, 40,  3, 48 };

	if ( ((curPs = check_pinset(&oldPs)) != 0) 	|| 
	     ((curPs = check_pinset(&newPs)) != 0)	)
	{
		gpio_pad_select_gpio(curPs->led_pin);
		gpio_set_direction(curPs->led_pin, GPIO_MODE_OUTPUT);
		gpio_set_level(curPs->led_pin, 1);

		gpio_set_pull_mode(curPs->clk_pin, GPIO_PULLUP_ONLY);
		gpio_set_pull_mode(curPs->cmd_pin, GPIO_PULLUP_ONLY);
		gpio_set_pull_mode(curPs->d0_pin, GPIO_PULLUP_ONLY);

		slot_config.clk 	= curPs->clk_pin;
		slot_config.cmd 	= curPs->cmd_pin;
		slot_config.d0 		= curPs->d0_pin;
		slot_config.width 	= 1;

		// SDMMC_FREQ_DEFAULT = 20MHz — стандартная скорость для GPIO Matrix.
		// 40MHz (HIGHSPEED) через GPIO Matrix + USB-нагрузка даёт end-bit error (0x8008).
		// input_delay_phase работает только при HIGHSPEED/52M, при 20MHz не нужна.
		host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
		host.input_delay_phase = SDMMC_DELAY_PHASE_1;

		if ((*host.init)() == ESP_OK)
		{
		//deinit() needs to be called to revert the init
			host_inited = true;

			//If this failed (indicated by card_handle != -1), slot deinit needs to called()
			//leave card_handle as is to indicate that (though slot deinit not implemented yet.
			if (sdmmc_host_init_slot(host.slot, &slot_config) == ESP_OK)
			{
				result = ESP_OK;
			}
			else
				ESP_LOGE(TAG, "slot init failed");
		}
		else
			ESP_LOGE(TAG, "host init failed");
	}

	return result;
}

int testsd_init_card() 
{
	int 			result = ESP_FAIL;

	ESP_LOGD(TAG, "Opening SD card for testing");

	if (card) {
		free(card);
		card = NULL;
	}

	if ((card = (sdmmc_card_t*)calloc(1, sizeof(sdmmc_card_t))) != 0)
	{
		// probe and initialize card
		if (sdmmc_card_init(&host, card) == ESP_OK)
		{
			result = ESP_OK;
		}
		else
			ESP_LOGE(TAG, "sdmmc_card_init failed");
	}
	else
		ESP_LOGE(TAG, "card alloc failed");

	return result;
}
static void testRead(PTESTSD_CONFIG c, 
					 PTESTSCORE 	ts)
{
	unsigned long remain = ts->total - ts->current;
	unsigned long count = (remain < ts->chunkSectors) ? remain : ts->chunkSectors;

	if (sdmmc_read_sectors(card, ts->readBuffer, ts->current, count) != ESP_OK)
	{
		ESP_LOGE(TAG, "Error reading blocks %lu..%lu", ts->current, ts->current + count - 1);
		ts->erroneous++;
	}
	ts->current += count - 1; /* caller adds 1 */
}
static void testReadWrite(PTESTSD_CONFIG c, 
					      PTESTSCORE 	ts)
{
	unsigned long remain = ts->total - ts->current;
	unsigned long count = (remain < ts->chunkSectors) ? remain : ts->chunkSectors;

	if (sdmmc_read_sectors(card, ts->readBuffer, ts->current, count) == ESP_OK)
	{
		if (sdmmc_write_sectors(card, ts->readBuffer, ts->current, count) != ESP_OK)
		{
			ESP_LOGE(TAG, "Error writing blocks %lu..%lu", ts->current, ts->current + count - 1);
			ts->erroneous++;
		}
	}
	else
	{
		ESP_LOGE(TAG, "Error reading blocks %lu..%lu", ts->current, ts->current + count - 1);
		ts->erroneous++;
	}
	ts->current += count - 1;
}
static void testReadBack(PTESTSD_CONFIG c, 
					     PTESTSCORE 	ts)
{
	unsigned long remain = ts->total - ts->current;
	unsigned long count = (remain < ts->chunkSectors) ? remain : ts->chunkSectors;
	size_t bytes = count * ts->blockSize;

	if (!ts->compareBuffer)	
	{
		if ((ts->compareBuffer = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT)) == NULL)
		{
			ESP_LOGE(TAG, "Error allocating memory for compare buffer");
			ts->erroneous = c->maxErrors;
		}
	}

	if (ts->compareBuffer)
	{
		if (sdmmc_read_sectors(card, ts->readBuffer, ts->current, count) == ESP_OK)
		{
			if (sdmmc_write_sectors(card, ts->readBuffer, ts->current, count) == ESP_OK)
			{
				if (sdmmc_read_sectors(card, ts->compareBuffer, ts->current, count) == ESP_OK)
				{
					if (memcmp(ts->compareBuffer, ts->readBuffer, bytes))
					{
						ESP_LOGE(TAG, "Error comparing blocks %lu..%lu", ts->current, ts->current + count - 1);
						ts->erroneous++;
					}
				}
				else
				{
					ESP_LOGE(TAG, "Error reading back blocks %lu..%lu", ts->current, ts->current + count - 1);
					ts->erroneous++;
				}
			}
			else
			{
				ESP_LOGE(TAG, "Error writing blocks %lu..%lu", ts->current, ts->current + count - 1);
				ts->erroneous++;
			}
		}
		else
		{
			ESP_LOGE(TAG, "Error reading blocks %lu..%lu", ts->current, ts->current + count - 1);
			ts->erroneous++;
		}
	}
	ts->current += count - 1;
}

void secondsToHMS(uint32_t total_seconds, char* buf, size_t size) 
{
    unsigned h = total_seconds / 3600;
    unsigned m = (total_seconds % 3600) / 60;
    unsigned s = total_seconds % 60;
    snprintf(buf, size, "%02u:%02u:%02u", h, m, s);
}

void testsd_task(void *arg)
{
	PTESTSD_CONFIG		c 					= calloc(1, sizeof(TESTSD_CONFIG));
	STDCOMMAND_PARAMS 	params 				= {0};
    int 				slot_num 			= *(int *)arg;
	TickType_t 			lastProgressTime 	= 0;
	int 				lastProgressPercent = -1;
	TickType_t 			lastDetectTime 		= 0;
	TESTSCORE			ts					= { 0 };

	ESP_LOGW(TAG, "Slot:%d task started", slot_num);

	me_state.command_queue[slot_num] = xQueueCreate(15, sizeof(command_message_t));

	configure_testsd(c, slot_num);

    if ( (1 == slot_num) && (ESP_OK == me_state.sd_init_res) )
	{
        mblog(E, "SD card already mounted on SLOT_1. Please remove SD card and reboot moduleBox");
        vTaskDelay(200);
        vTaskDelete(NULL);
    }

	waitForWorkPermit(slot_num);

	if (testsd_init() != ESP_OK) {
		ESP_LOGW(TAG, "Slot:%d SDMMC host init failed, detection and testing unavailable until retry", slot_num);
	}

	ESP_LOGW(TAG, "Slot:%d entering main loop", slot_num);

    while (1) 
	{
		/* Периодическая детекция SD-карты в состоянии простоя */
		if (ts.state != STATE_running && ts.state != STATE_init && ts.state != STATE_formatting) {
			TickType_t now = xTaskGetTickCount();
			if (pdTICKS_TO_MS(now - lastDetectTime) >= 1000) {
				lastDetectTime = now;
				int detected = detect_sd_card_presence();
				if (detected != c->cardDetected) {
					c->cardDetected = detected;
					if (c->ledPin != 0) {
						gpio_set_level(c->ledPin, !detected);
					}
					stdreport_s(c->stateReport, detected ? "inserted" : "ejected");
					ESP_LOGI(TAG, "Slot:%d SD card %s", slot_num, detected ? "inserted" : "ejected");
				}
			}
		}

        switch (stdcommand_receive(&c->cmds, &params, 0))
 		{
            case -1: // none
				if (ts.state != STATE_running)
				{
					vTaskDelay(50);
				}
                break;

            case rtpCMD_start:
				ESP_LOGD(TAG, "Got START command.");
				switch (ts.state)
				{
					case STATE_init:
					case STATE_running:
						ESP_LOGD(TAG, "... Aber bereits begonnen");
						break;

					default:
						ts.state 		= STATE_init;
						ts.result 		= 0;
						break;
				}
				stdreport_s(c->stateReport, (char*)getStateName(ts.state));
				ESP_LOGD(TAG, "Current state: %s", getStateName(ts.state));
                break;

            case rtpCMD_stop:
				ESP_LOGD(TAG, "Got STOP command.");
				switch (ts.state)
				{
					case STATE_stopped:
						ESP_LOGD(TAG, "... Aber bereits gestoppt");
						break;

					case STATE_done:
						ESP_LOGD(TAG, "last result: %d", ts.result);
						stdreport_s(c->resultReport, (char*)getResultName(ts.result));

						ts.state 		= STATE_stopped;
					break;

					default:
						ts.state 		= STATE_stopped;
						break;
				}
				stdreport_s(c->stateReport, (char*)getStateName(ts.state));
				ts.result 	= 0;
				ESP_LOGD(TAG, "Current state: %s", getStateName(ts.state));
                break;

			default:
				ESP_LOGW(TAG, "Slot:%d unknown command: %s", slot_num, c->cmds.msg.str);
				break;				
		}

		switch (ts.state)
		{
			case STATE_init:
				{
					ts.result 	= 0;

					if (!host_inited) {
						ESP_LOGW(TAG, "Slot:%d retrying SDMMC host init", slot_num);
						if (testsd_init() != ESP_OK) {
							ESP_LOGE(TAG, "SDMMC host init failed, cannot test");
							ts.result = -1;
							ts.state = STATE_done;
							stdreport_s(c->resultReport, (char*)getResultName(ts.result));
							stdreport_s(c->stateReport, (char*)getStateName(ts.state));
							break;
						}
					}

					if (testsd_init_card() == ESP_OK)
					{
						ts.beginTime	= xTaskGetTickCount();
    					ts.blockSize 	= card->csd.sector_size;
    					ts.total 		= card->csd.capacity;
						//ts.total 		= 1000;
						ts.current 		= 0;
						ts.erroneous 	= 0;
						ts.chunkSectors = CHUNK_SECTORS;

						/* Пытаемся выделить DMA-буфер, уменьшая chunk при нехватке памяти */
						ts.readBuffer = NULL;
						while (ts.chunkSectors >= 1 && !ts.readBuffer) {
							ts.readBuffer = heap_caps_malloc(ts.blockSize * ts.chunkSectors, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
							if (!ts.readBuffer) {
								ts.chunkSectors /= 2;
							}
						}

						if (ts.readBuffer != NULL)
						{
							ESP_LOGI(TAG, "Begining %s test of SD-card: %lu sectors, %u bytes/sector, chunk=%lu sectors (%lu bytes)", 
										testTypeNames[c->testType],
										(unsigned long)card->csd.capacity, (unsigned)ts.blockSize,
										ts.chunkSectors, ts.chunkSectors * ts.blockSize);

							ts.state = STATE_running;
						}
						else
						{
							ESP_LOGE(TAG, "Error allocating memory for test buffer, test done.");
							ts.state = STATE_done;
						}
					}
					else
					{
						ESP_LOGE(TAG, "Error opening SD-card, test done.");
						ts.state = STATE_done;
					}

					stdreport_s(c->resultReport, (char*)getResultName(ts.result));
					stdreport_s(c->stateReport, (char*)getStateName(ts.state));
				}
				break;

			case STATE_running:	

				switch (c->testType)
				{
					case TESTT_read:		testRead(c, &ts);			break;
					case TESTT_readwrite:	testReadWrite(c, &ts);		break;
					case TESTT_readback:	testReadBack(c, &ts);		break;
				
					default:
						break;
				}

				ts.current++;

				{
					int pct = (int)((ts.current * 100) / ts.total);
					if (pct != lastProgressPercent) {
						lastProgressPercent = pct;
						stdreport_i(c->progressReport, pct);
					}
				}

				if (lastProgressTime)
				{
					TickType_t now = xTaskGetTickCount();

					if (pdTICKS_TO_MS(now - lastProgressTime) >= 10000)
					{
						ESP_LOGI(TAG, "Running: %d%% (%lu / %lu)", lastProgressPercent, ts.current, ts.total);

						lastProgressTime = xTaskGetTickCount();
					}
				}
				else
					lastProgressTime = xTaskGetTickCount();

		
				if (ts.erroneous >= c->maxErrors)
				{
					ESP_LOGE(TAG, "Errors threshold exceeded, test was stopped %lu/%lu", ts.erroneous, ts.total);

					ts.result 	= -1;
					ts.state 	= STATE_done;

					stdreport_s(c->resultReport, (char*)getResultName(ts.result));
					stdreport_s(c->stateReport, (char*)getStateName(ts.state));
					break;
				}

				if (ts.current >= ts.total)
				{
					secondsToHMS(pdTICKS_TO_MS(xTaskGetTickCount() - ts.beginTime) / 1000, ts.readBuffer, ts.blockSize);

					if (ts.erroneous)
						ESP_LOGE(TAG, "Test done, errors threshold does not exeeded: %lu vs %u, duration: %s", ts.erroneous, c->maxErrors, ts.readBuffer);
					else
						ESP_LOGI(TAG, "Test done without errors, duration: %s", ts.readBuffer);

					ts.result 	= 1;
					ts.state 	= STATE_formatting;

					stdreport_s(c->stateReport, (char*)getStateName(ts.state));
				}
				if (c->runDelay)
				{
					vTaskDelay(c->runDelay);
				}
				break;

			case STATE_formatting:
				if (format_sd_card() == ESP_OK) {
					ESP_LOGI(TAG, "Card formatted after successful test");
					ts.result = 1;
				} else {
					ESP_LOGE(TAG, "Card formatting failed");
					ts.result = -2;
				}
				ts.state = STATE_done;
				stdreport_s(c->resultReport, (char*)getResultName(ts.result));
				stdreport_s(c->stateReport, (char*)getStateName(ts.state));
				break;

			default:
				break;
		}
		
		if ( (ts.state != STATE_running) && (ts.state != STATE_init) && (ts.state != STATE_formatting) && (ts.readBuffer != 0) )
		{
			if (ts.compareBuffer)
			{
				free(ts.compareBuffer);
				ts.compareBuffer = NULL;
			}

			free(ts.readBuffer);
			ts.readBuffer = NULL;

			if (card)
			{
				free(card);
				card = NULL;
			}

			/* Сброс SDMMC-хоста после теста для очистки состояния периферии.
			   Без этого ошибка 0x107 остаётся в хосте и ломает последующую детекцию. */
			if (host_inited) {
				sdmmc_host_deinit();
				host_inited = false;
			}
			if (testsd_init() != ESP_OK) {
				ESP_LOGW(TAG, "Slot:%d SDMMC host reinit failed after test", slot_num);
			}

			ts.state = STATE_stopped;
			stdreport_s(c->stateReport, (char*)getStateName(ts.state));

			ESP_LOGI(TAG, "Test resources freed, ready for new start command");
		}
    }
}
void start_testsd_task(int slot_num){
	uint32_t heapBefore = xPortGetFreeHeapSize();
	static int t_slot_num;
	t_slot_num = slot_num;
	xTaskCreatePinnedToCore(testsd_task, "testsd_task", 1024 * 4, &t_slot_num, 12, NULL,1);

	ESP_LOGD(TAG, "TESTSD init ok: %d Heap usage: %lu free heap:%u", slot_num, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
}
const char * get_manifest_testsd()
{
	return manifesto;
}
