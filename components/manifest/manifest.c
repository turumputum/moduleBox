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
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

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
"                      \"default\": \"moduleBox\" },\n"
"                    { \"key\" : \"statusPeriod\",\n"
"                      \"default\" : \"900\" },\n"
"                    { \"key\" : \"statusAllChannels\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  },\n"
"                    { \"key\" : \"logLevel\",\n"
"                      \"enum\" : [ \"none\", \"error\", \"warn\", \"info\", \"debug\", \"verbose\" ]  },\n"
"                    { \"key\" : \"logMaxSize\",\n"
"                      \"default\" : \"0\" },\n"
"                    { \"key\" : \"logChapters\",\n"
"                      \"default\" : \"\" },\n"
"                    { \"key\" : \"boardVersion\",\n"
"                      \"default\" : \"0\" },\n"
"                    { \"key\" : \"USB_debug\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  }\n"
"                ]\n"
"        },\n"
"        { \"chapter\" : \"LAN\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"LAN_enable\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  },\n"
"                    { \"key\" : \"DHCP\",\n"
"                      \"enum\" : [ \"false\", \"true\" ]  },\n"
"                    { \"key\" : \"ipAdress\",\n"
"                      \"default\" : \"192.168.1.100\" },\n"
"                    { \"key\" : \"netMask\",\n"
"                      \"default\" : \"255.255.255.0\" },\n"
"                    { \"key\" : \"gateWay\",\n"
"                      \"default\" : \"192.168.1.1\" }\n"
"                ]\n"
"        },\n"
"        { \"chapter\" : \"UDP\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"serverAdress\",\n"
"                      \"default\" : \"192.168.1.55\" },\n"
"                    { \"key\" : \"serverPort\",\n"
"                      \"default\" : \"9000\" },\n"
"                    { \"key\" : \"myPort\",\n"
"                      \"default\" : \"9000\" },\n"
"                    { \"key\" : \"crosslink\",\n"
"                      \"default\" : \"\" }\n"
"                ]\n"
"        },\n"
"        { \"chapter\" : \"MQTT\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"mqttBrokerAdress\",\n"
"                      \"default\" : \"\" },\n"
"                    { \"key\" : \"mqttLogin\",\n"
"                      \"default\" : \"\" },\n"
"                    { \"key\" : \"mqttPass\",\n"
"                      \"default\" : \"\" },\n"
"                    { \"key\" : \"mqttQOS\",\n"
"                      \"default\" : \"0\",\n"
"                      \"enum\" : [ \"0\", \"1\", \"2\" ]  },\n"
"                    { \"key\" : \"mqttWatchdogTimeout\",\n"
"                      \"default\" : \"0\" },\n"
"                    { \"key\" : \"mqttTLS\",\n"
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
	const char * 	    tmp;
    GET_MANIFEST_FUNC   f;

    if (!checkExistent())
    {
        // IDF 5.5.4 FATFS портит файлы >64КБ при многих мелких write()-
        // (манифест ~130КБ)- Собираем весь JSON в один буфер и пишем
        // одним POSIX write()- stdio fwrite не годится- он буферизует и всё
        // равно дробит на мелкие записи-
        static const char * const trailer = "\n]\n}\n";

        // 1й проход- считаем итоговый размер
        size_t total = strlen(configDescription);
        for (int i = 0; (f = funcs[i]) != NULL; i++)
        {
            if ((tmp = (*f)()) != NULL)
            {
                total += (0 == i ? 1 : 2) + strlen(tmp);   // разделитель '\n' или ',\n'
            }
        }
        total += strlen(trailer);

        // буфер >16КБ уходит в PSRAM (не давит на internal RAM)
        char * buf = malloc(total + 1);
        if (buf == NULL)
        {
            ESP_LOGE(TAG, "manifest save error: out of memory (%u bytes)", (unsigned)(total + 1));
            return ESP_FAIL;
        }

        // 2й проход- собираем единый буфер
        char * p = buf;
        size_t n = strlen(configDescription);
        memcpy(p, configDescription, n); p += n;
        for (int i = 0; (f = funcs[i]) != NULL; i++)
        {
            if ((tmp = (*f)()) != NULL)
            {
                const char * sep = (0 == i) ? "\n" : ",\n";
                size_t sl = strlen(sep);
                memcpy(p, sep, sl); p += sl;
                size_t tl = strlen(tmp);
                memcpy(p, tmp, tl); p += tl;
            }
        }
        n = strlen(trailer);
        memcpy(p, trailer, n); p += n;

        size_t buflen = (size_t)(p - buf);

        // один write() => FATFS получает весь файл одной f_write- обходим баг
        int fd = open(MANIFESTO_FULL_FNAME, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
        {
            ssize_t written = write(fd, buf, buflen);
            close(fd);
            if (written == (ssize_t)buflen)
            {
                ESP_LOGI(TAG, "manifest saved (%u bytes, single write)", (unsigned)buflen);
                result = ESP_OK;
            }
            else
            {
                ESP_LOGE(TAG, "manifest write short: %d/%u", (int)written, (unsigned)buflen);
            }
        }
        else
        {
            ESP_LOGE(TAG, "manifest save error: open() failed");
        }

        free(buf);
    }
    else
    {
        ESP_LOGI(TAG, "manifest exists, save skiped");
        result = ESP_OK;
    }

	return result;
}
