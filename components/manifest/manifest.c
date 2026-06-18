// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdint.h>
#include <stdio.h>

#include "esp_peripherals.h"

/* Принудительно включаем DEBUG-уровень для этого файла, чтобы видеть
   диагностику ESP_LOGD сразу после прошивки без правки sdkconfig. */
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include "esp_log.h"
#include "stateConfig.h"

#include <manifest.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define MAX_FILES_TO_DELETE 10  // if more - than next time

#define MOUNT_POINT             "/sdcard"
#define MANIFESTO_FNAME_BASE    "manifest-" 
#define MANIFESTO_FNAME         MANIFESTO_FNAME_BASE VERSION ".json"
#define MANIFESTO_FULL_FNAME    MOUNT_POINT "/" MANIFESTO_FNAME

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

static const char *TAG      = "MAIN";


static const char * configDescription = 
"{ \"config\" : [\n"
"        { \"chapter\" : \"SYSTEM\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"deviceName\",\n"
"                      \"description\" : \"Имя устройства в сети\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"moduleBox\" },\n"
"                    { \"key\" : \"statusPeriod\",\n"
"                      \"description\" : \"Период отправки статуса, 0 - отключено\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"unit\" : \"s\",\n"
"                      \"valueDefault\" : \"900\" },\n"
"                    { \"key\" : \"statusAllChannels\",\n"
"                      \"description\" : \"Отправлять статус по всем каналам\",\n"
"                      \"valueType\" : \"flag\",\n"
"                      \"valueDefault\" : \"true\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  },\n"
"                    { \"key\" : \"logLevel\",\n"
"                      \"description\" : \"Уровень логирования\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"warn\",\n"
"                      \"enum\" : [ \"none\", \"error\", \"warn\", \"info\", \"debug\", \"verbose\" ]  },\n"
"                    { \"key\" : \"cleanLogOnStart\",\n"
"                      \"description\" : \"Стереть все логи при старте\",\n"
"                      \"valueType\" : \"flag\",\n"
"                      \"valueDefault\" : \"false\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  },\n"
"                    { \"key\" : \"boardVersion\",\n"
"                      \"description\" : \"Версия платы\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"valueDefault\" : \"3\" },\n"
"                    { \"key\" : \"USB_debug\",\n"
"                      \"description\" : \"Вывод отладки в USB-CDC\",\n"
"                      \"valueType\" : \"flag\",\n"
"                      \"valueDefault\" : \"false\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  },\n"
"                    { \"key\" : \"crossLinkDebug\",\n"
"                      \"description\" : \"Печать срабатываний кросслинкера в crossLink/execute\",\n"
"                      \"valueType\" : \"flag\",\n"
"                      \"valueDefault\" : \"false\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  }\n"
"                ]\n"
"        },\n"
"        { \"chapter\" : \"LAN\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"LAN_enable\",\n"
"                      \"description\" : \"Включить проводную сеть\",\n"
"                      \"valueType\" : \"flag\",\n"
"                      \"valueDefault\" : \"false\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  },\n"
"                    { \"key\" : \"DHCP\",\n"
"                      \"description\" : \"Получать адрес по DHCP\",\n"
"                      \"valueType\" : \"flag\",\n"
"                      \"valueDefault\" : \"true\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  },\n"
"                    { \"key\" : \"ipAdress\",\n"
"                      \"description\" : \"IP адрес устройства\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"192.168.1.100\" },\n"
"                    { \"key\" : \"netMask\",\n"
"                      \"description\" : \"Маска подсети\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"255.255.255.0\" },\n"
"                    { \"key\" : \"gateWay\",\n"
"                      \"description\" : \"Адрес шлюза\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"192.168.1.1\" }\n"
"                ]\n"
"        },\n"
"        { \"chapter\" : \"UDP\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"serverAdress\",\n"
"                      \"description\" : \"IP адрес UDP сервера\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"192.168.1.55\" },\n"
"                    { \"key\" : \"serverPort\",\n"
"                      \"description\" : \"Порт UDP сервера\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"valueDefault\" : \"9000\" },\n"
"                    { \"key\" : \"myPort\",\n"
"                      \"description\" : \"Локальный UDP порт устройства\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"valueDefault\" : \"9000\" },\n"
"                    { \"key\" : \"crosslink\",\n"
"                      \"description\" : \"Кросслинк по UDP\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"\" }\n"
"                ]\n"
"        },\n"
"        { \"chapter\" : \"OSC\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"oscServerAdress\",\n"
"                      \"description\" : \"IP адрес OSC сервера\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"192.168.1.55\" },\n"
"                    { \"key\" : \"oscServerPort\",\n"
"                      \"description\" : \"Порт OSC сервера\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"valueDefault\" : \"9000\" },\n"
"                    { \"key\" : \"oscMyPort\",\n"
"                      \"description\" : \"Локальный OSC порт устройства\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"valueDefault\" : \"9000\" }\n"
"                ]\n"
"        },\n"
"        { \"chapter\" : \"MQTT\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"mqttBrokerAdress\",\n"
"                      \"description\" : \"Адрес MQTT брокера\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"\" },\n"
"                    { \"key\" : \"mqttLogin\",\n"
"                      \"description\" : \"Логин для подключения к MQTT брокеру\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"\" },\n"
"                    { \"key\" : \"mqttPass\",\n"
"                      \"description\" : \"Пароль для подключения к MQTT брокеру\",\n"
"                      \"valueType\" : \"string\",\n"
"                      \"valueDefault\" : \"\" },\n"
"                    { \"key\" : \"mqttQOS\",\n"
"                      \"description\" : \"Качество обслуживания MQTT\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"valueDefault\" : \"0\",\n"
"                      \"enum\" : [ \"0\", \"1\", \"2\" ]  },\n"
"                    { \"key\" : \"mqttWatchdogTimeout\",\n"
"                      \"description\" : \"Таймаут сторожевого таймера MQTT, 0 - отключено\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"unit\" : \"s\",\n"
"                      \"valueDefault\" : \"0\" },\n"
"                    { \"key\" : \"mqttKeepAlive\",\n"
"                      \"description\" : \"Интервал keep-alive MQTT сессии\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"unit\" : \"s\",\n"
"                      \"valueDefault\" : \"60\" },\n"
"                    { \"key\" : \"mqttTLS\",\n"
"                      \"description\" : \"Использовать TLS для подключения к MQTT\",\n"
"                      \"valueType\" : \"flag\",\n"
"                      \"valueDefault\" : \"false\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  }\n"
"                ]\n"
"        },\n"
"        { \"chapter\" : \"SLOT_*\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"mode\",\n"
"                      \"spec\" : \"slot_mode\" },\n"
"                    { \"key\" : \"options\",\n"
"                      \"spec\" : \"slot_options\" },\n"
"                    { \"key\" : \"crosslink\",\n"
"                      \"spec\" : \"slot_crosslink\" }\n"
"                ]\n"
"        }\n"
"],\n"
"\"modes\" : [\n"
"        {\n"
"                \"mode\": \"empty\",\n"
"                \"slots\": \"0-11\",\n"
"                \"description\": \"Пустой слот\",\n"
"                \"options\": []\n"
"        },\n"
"        {\n"
"                \"mode\": \"SD_card\",\n"
"                \"slots\": \"1\",\n"
"                \"description\": \"Карта SD\",\n"
"                \"options\": []\n"
"        },";



#include "manifest_modules.h"

#define MODDEF(a) extern const char * get_manifest_##a();

MODULE_FUNCTIONS_DEFS


#define MOD(a) get_manifest_##a

typedef const char *        (*GET_MANIFEST_FUNC)();


static GET_MANIFEST_FUNC    funcs   [] = 
{
    MODULE_FUNCTIONS
    NULL
}; 

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

bool checkExistent()
{
    bool        result          = false;
	FF_DIR      dir;
	FILINFO     fno;
    int         filesToDelete   = 0;
    char *      toDelete        [ MAX_FILES_TO_DELETE ];
    char        tmp             [ 255 ];


	if (f_opendir(&dir, "/") == FR_OK)
    {
		while ((f_readdir(&dir, &fno) == FR_OK) && fno.fname[0])
        {
            if (!(fno.fattrib & AM_DIR))
            {
                if (strstr(fno.fname, MANIFESTO_FNAME_BASE))
                {
                    if (!strcasecmp(fno.fname, MANIFESTO_FNAME))
                    {
                        struct stat st;
                        if (stat(MANIFESTO_FULL_FNAME, &st) != 0 || st.st_size < 1024) {
                            ESP_LOGW(TAG, "manifest file too small (%ld bytes), will recreate",
                                     (long)(stat(MANIFESTO_FULL_FNAME, &st) == 0 ? st.st_size : -1));
                        } else {
                            /* Размер ок — валидируем содержимое.
                               FAT может оставить файл с NUL-префиксом если
                               power-cut пришёл между fopen('w') и fclose:
                               кластеры аллоцированы, но данные не легли. */
                            FILE *f = fopen(MANIFESTO_FULL_FNAME, "r");
                            if (f != NULL) {
                                char head[4] = {0};
                                size_t got = fread(head, 1, sizeof(head) - 1, f);
                                fclose(f);
                                if (got > 0 && head[0] == '{') {
                                    result = true;
                                } else {
                                    ESP_LOGW(TAG, "manifest corrupted (head: %02x %02x %02x), will recreate",
                                             (unsigned char)head[0], (unsigned char)head[1], (unsigned char)head[2]);
                                }
                            } else {
                                ESP_LOGW(TAG, "manifest cannot be reopened for validation, will recreate");
                            }
                        }
                    }
                    else
                    {
                        if (filesToDelete < MAX_FILES_TO_DELETE)
                        {
                            toDelete[filesToDelete] = strdup(fno.fname);

                            filesToDelete++;
                        }
                    }
                }
            }
        }

        f_closedir(&dir);

        if (filesToDelete)
        {
            for (int i = 0; i < filesToDelete; i++)
            {
                ESP_LOGI(TAG, "deleting old manifest: %s", toDelete[i]);

                sprintf(tmp, "%s/%s", MOUNT_POINT, toDelete[i]);
                remove(tmp);
                free(toDelete[i]);
            }
        }
    }

    return result;
}
/* Пишет манифест на SD-карту через ОДИН большой write().
   Workaround для бага в FATFS/SDMMC IDF v5.5.4: для файлов >~64 КБ
   серия мелких write()'ов обновляет директорию (правильный size),
   но первый кластер остаётся незаписанным (NUL). Один большой
   write() уходит на SD как multi-block transfer и работает корректно. */
static int _writeManifestoOnce(const char *path)
{
    const char *        tmp;
    GET_MANIFEST_FUNC   f;
    remove(path);

    /* Считаем общий размер: configDescription + (sep + module_i) * N + tail */
    size_t totalSize = strlen(configDescription);
    for (int i = 0; (f = funcs[i]) != NULL; i++) {
        if ((tmp = (*f)()) != NULL) {
            totalSize += (0 == i) ? 1 : 2;
            totalSize += strlen(tmp);
        }
    }
    totalSize += 5; /* "\n]\n}\n" */

    /* Большой буфер — пытаемся в SPIRAM, fallback в internal heap */
    char *bigBuf = heap_caps_malloc(totalSize + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bigBuf) bigBuf = heap_caps_malloc(totalSize + 16, MALLOC_CAP_8BIT);
    if (!bigBuf) {
        ESP_LOGE(TAG, "manifest: cannot allocate %zu byte buffer", totalSize);
        return ESP_FAIL;
    }

    size_t pos = 0;
    size_t cdLen = strlen(configDescription);
    memcpy(bigBuf + pos, configDescription, cdLen); pos += cdLen;
    for (int i = 0; (f = funcs[i]) != NULL; i++) {
        if ((tmp = (*f)()) != NULL) {
            const char *sep = (0 == i) ? "\n" : ",\n";
            size_t slen = strlen(sep);
            memcpy(bigBuf + pos, sep, slen); pos += slen;
            size_t tlen = strlen(tmp);
            memcpy(bigBuf + pos, tmp, tlen); pos += tlen;
        }
    }
    memcpy(bigBuf + pos, "\n]\n}\n", 5); pos += 5;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        ESP_LOGE(TAG, "manifest open('%s') failed errno=%d", path, errno);
        heap_caps_free(bigBuf);
        return ESP_FAIL;
    }

    ssize_t w = write(fd, bigBuf, pos);
    fsync(fd);
    close(fd);
    heap_caps_free(bigBuf);

    if (w != (ssize_t)pos) {
        ESP_LOGE(TAG, "manifest write short: %zd of %zu", w, pos);
        return ESP_FAIL;
    }

    /* Verify: читаем первые 3 байта чтобы поймать NUL-corruption если
       баг вернётся (например после обновления IDF). */
    int rfd = open(path, O_RDONLY);
    if (rfd >= 0) {
        unsigned char head[4] = {0};
        ssize_t got = read(rfd, head, 3);
        close(rfd);
        if (got > 0 && head[0] == '{') return ESP_OK;
        ESP_LOGE(TAG, "manifest verify: head=%02x %02x %02x", head[0], head[1], head[2]);
    }
    return ESP_FAIL;
}

int saveManifesto()
{
    int result = ESP_FAIL;

    if (!checkExistent())
    {
        result = _writeManifestoOnce(MANIFESTO_FULL_FNAME);
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "manifest saved");
        } else {
            ESP_LOGE(TAG, "manifest save failed");
        }
    }
    else
    {
        ESP_LOGI(TAG, "manifest exists, save skiped");
        result = ESP_OK;
    }

    return result;
}
