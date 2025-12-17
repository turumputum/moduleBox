// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************


#include <scheduler.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "esp_netif_sntp.h"
#include "lwip/ip_addr.h"
#include "esp_sntp.h"
#include "stateConfig.h"
#include <mbdebug.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern configuration me_config;
extern stateStruct me_state;
static const char *TAG = "MAIN";
static bool flagTimeSyncd = false;


// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

extern void crosslinker(char* str);


static void time_sync_notification_cb(struct timeval *tv) {
	ESP_LOGD(TAG, "SCHEDULE: time is syncronized");
}

void sntp_sync_task()
{
    time_t now;
    struct tm timeinfo;
    char strftime_buf[64];

    if (ESP_OK != me_state.LAN_init_res)
    {
        ESP_LOGI(TAG, "SCHEDULE: wait for network ... ");    

        while (ESP_OK != me_state.LAN_init_res)
        {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(me_config.ntpServer);
    config.start = true;                       // start SNTP service explicitly (after connecting)
    config.server_from_dhcp = false;             // accept NTP offers from DHCP server, if any (need to enable *before* connecting)
    config.renew_servers_after_new_IP = false;   // let esp-netif update configured SNTP server(s) after receiving DHCP lease
    config.index_of_first_server = 0;           // updates from server num 1, leaving server 0 (from DHCP) intact
    config.sync_cb = time_sync_notification_cb; // only if we need the notification function
    esp_netif_sntp_init(&config);
    esp_netif_sntp_start();

    ESP_LOGD(TAG, "SCHEDULE: sync with NTP server '%s' ...", me_config.ntpServer);

    // wait for time to be set
    int retry = 0;
    const int retry_count = 30;
    sntp_sync_status_t result;
    while (((result = sntp_get_sync_status()) != SNTP_SYNC_STATUS_COMPLETED) && (++retry < retry_count)) {
        //ESP_LOGD(TAG, "SCHEDULE: SNTP sync take %d/%d ...", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }

    switch (result)
    {
        case SNTP_SYNC_STATUS_COMPLETED:
            time(&now);
            localtime_r(&now, &timeinfo);
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "SCHEDULE: now is %s", strftime_buf);

            mblog(I, "SCHEDULE: time synchronized");
            flagTimeSyncd = true;
            break;
        
        default:
            mblog(W, "SCHEDULE: cannot synchronize time, skip scheduling");
            break;
    }

    esp_sntp_stop();

    vTaskDelete(NULL);
}
const char* day_of_week_to_string(day_of_week_t day) {
    const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (day >= 0 && day <= 6)
        return days[day];
    return "Unknown";
}

void scheduler_periodic_turn()
{
    if (flagTimeSyncd && (me_config.scheduleCount > 0))
    {
        for (unsigned i = 0; i < me_config.scheduleCount; i++)
        {
            time_t now;
            struct tm timeinfo;

            time(&now);
            gmtime_r(&now, &timeinfo);
            
            if (matches_schedule_time(&me_config.scheduleEntries[i], 
                                      1900 + timeinfo.tm_year, timeinfo.tm_mon + 1, timeinfo.tm_mday, 
                                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, 
                                      timeinfo.tm_wday))
            {
                ESP_LOGD(TAG, "SCHEDULE: match #%d: %.4d-%.2d-%.2d %.2d:%.2d:%.2d %s >> '%s'", 
                                i + 1, 
                                1900 + timeinfo.tm_year, timeinfo.tm_mon + 1, timeinfo.tm_mday, 
                                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, 
                                      day_of_week_to_string(timeinfo.tm_wday),
                                      me_config.scheduleEntries[i].command
                                      );

                crosslinker(me_config.scheduleEntries[i].command);
            }
        }
    }
}
void start_scheduler_task()
{
    if (me_config.scheduleCount > 0)
    {
        time_t now;
        struct tm timeinfo;
        // Set timezone to Eastern Standard Time and print local time
        // setenv("TZ", "Europe/Moscow", 1);
        // tzset();
        time(&now);
        localtime_r(&now, &timeinfo);
        
        // Is time set? If not, tm_year will be (1970 - 1900).
        if (timeinfo.tm_year < (2025 - 1900))
        {
            ESP_LOGD(TAG, "SCHEDULE: Time is not set yet.");

            if (me_config.LAN_enable || me_config.WIFI_enable)
            {
                if (me_config.ntpServer)
                {
                    xTaskCreatePinnedToCore(sntp_sync_task, "sntp_sync_task", 1024 * 3, NULL, configMAX_PRIORITIES - 10, NULL,0);
                }
                else
                    mblog(W, "SCHEDULE: SNTP server not specified, skip scheduling");
            }
            else
                mblog(W, "SCHEDULE: network is not enabled, skip scheduling");
        }
        else
        {
            // "NO BATTERY" logic - if we already got time - we are online for sometime, and have already synchronized 
            // in the recent past. If moduleBox ever gets a battery, this logic must be changed for periodic re-sync.
            mblog(I, "SCHEDULE: RTC is set, no SNTP sync needed");
            flagTimeSyncd = true;
        }
    }
}

