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
"                    { \"key\" : \"loglevel\",\n"
"                      \"enum\" : [ \"none\", \"error\", \"warn\", \"info\", \"debug\", \"verbose\" ]  }\n"
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
"                    { \"key\" : \"udpServerAdress\",\n"
"                      \"default\" : \"192.168.1.55\" },\n"
"                    { \"key\" : \"udpServerPort\",\n"
"                      \"default\" : \"9000\" },\n"
"                    { \"key\" : \"udpMyPort\",\n"
"                      \"default\" : \"9000\" }\n"
"                ]\n"
"        },\n"
"        { \"chapter\" : \"MQTT\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"mqttBrokerAdress\",\n"
"                      \"default\" : \"192.168.1.55\" }\n"
"                ]\n"
"        },\n"
"        { \"chapter\" : \"SLOT_*\",\n"
"          \"values\" : [\n"
"                    { \"key\" : \"mode\",\n"
"                      \"spec\" : \"slot_mode\" },\n"
"                    { \"key\" : \"options\",\n"
"                      \"spec\" : \"slot_options\" },\n"
"                    { \"key\" : \"cross_link\",\n"
"                      \"spec\" : \"slot_crosslink\" }\n"
"                ]\n"
"        }\n"
"],\n"
"\"modes\" : [\n"
"        {\n"
"                \"mode\": \"empty\",\n"
"                \"description\": \"Пустой слот\",\n"
"                \"options\": []\n"
"        },";



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
                        result = true;
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
