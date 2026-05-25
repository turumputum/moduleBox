// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************

#include <stdint.h>
#include <stdio.h>

#include "esp_peripherals.h"
#include "esp_log.h"
#include "stateConfig.h"

#include <manifest.h>
#include <string.h>
#include <sys/stat.h>

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
"                    { \"key\" : \"logMaxSize\",\n"
"                      \"description\" : \"Максимальный размер файла лога, 0 - без лога\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"unit\" : \"bytes\",\n"
"                      \"valueDefault\" : \"0\" },\n"
"                    { \"key\" : \"logChapters\",\n"
"                      \"description\" : \"Выводить имена глав в логе\",\n"
"                      \"valueType\" : \"flag\",\n"
"                      \"valueDefault\" : \"1\" },\n"
"                    { \"key\" : \"boardVersion\",\n"
"                      \"description\" : \"Версия платы\",\n"
"                      \"valueType\" : \"int\",\n"
"                      \"valueDefault\" : \"3\" },\n"
"                    { \"key\" : \"USB_debug\",\n"
"                      \"description\" : \"Вывод отладки в USB-CDC\",\n"
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
                        if (stat(MANIFESTO_FULL_FNAME, &st) == 0 && st.st_size >= 1024) {
                            result = true;
                        } else {
                            ESP_LOGW(TAG, "manifest file too small (%ld bytes), will recreate", (long)st.st_size);
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
int saveManifesto()
{
	int 			    result 		= ESP_FAIL;
	FILE *			    manFile;
	const char * 	    tmp;
    GET_MANIFEST_FUNC   f;

    if (!checkExistent())
    {
        if ((manFile = fopen(MANIFESTO_FULL_FNAME, "w")) != NULL)
        {
            fprintf(manFile, "%s", configDescription);

            for (int i = 0; (f = funcs[i]) != NULL; i++)
            {
                if ((tmp = (*f)()) != NULL)
                {
                    fprintf(manFile, "%s%s", 0 == i ? "\n" : ",\n", tmp);
                }
            }

            fprintf(manFile, "%s", "\n]\n}\n");

            ESP_LOGI(TAG, "manifest saved");

            result = ESP_OK;
            
            fclose(manFile);
        }
        else
        {
            ESP_LOGE(TAG, "manifest save error: fopen() failed");
        }
    }
    else
    {
        ESP_LOGI(TAG, "manifest exists, save skiped");
        result = ESP_OK;
    }

	return result;
}
