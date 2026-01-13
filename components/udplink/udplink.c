// ***************************************************************************
// TITLE
//
// PROJECT
//     moduleBox
// ***************************************************************************


#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "esp_eth_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_efuse.h"

//#include <sys/socket.h>
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "moduleboxapp.h"

#include "stateConfig.h"

#include "LAN.h"
#include "ftp.h"
#include "myMqtt.h"
#include "tinyosc.h"
#include "reporter.h"
#include "executor.h"

#include "mdns.h"

#include <axstring.h>
#include <mbdebug.h>


// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define	DEFAULT_UDP_PORT	9000


// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

typedef struct __tag_UDPCROSSLINK
{
	char * 	name;
	char * 	rule;
} UDPCROSSLINK;

// ---------------------------------------------------------------------------
// ---------------------------------- DATA -----------------------------------
// -----|-------------------|-------------------------------------------------

extern configuration me_config;
extern stateStruct me_state;
extern void crosslinker(char* str);
extern void crosslinker_(char* str, char* rules);

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "[UDP]";

static 	char * 				udp_crosslink 	= nil;
static  int 				linksCount		= 0;
static 	UDPCROSSLINK		links	[ 30 ];

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

static void _cut_crlfs(char * value)
{
	char * on;

	if ((on = strchr(value, '\r')) != 0)
	{
		*on = 0;
	}

	if ((on = strchr(value, '\n')) != 0)
	{
		*on = 0;
	}
}
static char * _skip_spaces(char * value)
{
    unsigned char * on = (unsigned char *)value;
            
    while (*on && (*on <= ' '))
    {
        on++;
    }
                             
    return (char*)on;
}
int execute_links(char * buff)
{
	int 		result		= 0;
	char * 		response;
	char * 		name;
	char * 		event;
	char * 		on;
	char 		tmp 		[ 280 ];

	if (linksCount)
	{
		_cut_crlfs(buff);
		name = _skip_spaces(buff);

		if ((event = strchr(name, '/')) != 0)
		{
			*event = 0;
			event++;

			for (int i = 0; !result && (i < linksCount); i++)
			{
				if (!strcmp(name, links[i].name))
				{
					snprintf(tmp, sizeof(tmp) - 1, "%s/%s", me_config.deviceName, event);

					//printf("UDP crosslink: name '%s', event '%s', rule '%s'\n", name, tmp, links[i].rule);

					ESP_LOGD(TAG, "UDP crosslink: name '%s', event '%s', rule '%s'", name, tmp, links[i].rule);

					crosslinker_(tmp, links[i].rule);
					result = 1;
				}
			}
		}
	}

	return result;
}
static void parseUdpCrossLinks()
{
	if (*me_config.udp_crosslink)
	{
		if ((udp_crosslink	= strdup(me_config.udp_crosslink)) != nil)
		{
			char * on;
			char * begin = udp_crosslink;

			do
			{
				on = _skip_spaces(begin);

				if ((on = strchr(on, '/')) != nil)
				{
					*on = 0;
					on += 1;

					links[linksCount].name 		= begin;
					links[linksCount].rule 		= on;

					linksCount++;
				}
				else
					on = begin;

				if ((begin = strchr(on, ',')) != nil)
				{
					*begin = 0;
					begin++;
				}

			} while (begin);

			// printf("UDP cross links: %d\n", linksCount);
			// for (int i = 0; i < linksCount; i++)
			// {
			// 	printf("name: %s rule: '%s'\n", links[i].name, links[i].rule);
			// }
		}
	}
}
void udplink_task()
{
	int len;

	if (me_config.udpMyPort)
	{
		// Trying to extract port number from the server definition
		if (*me_config.udpServerAdress)
		{
			char * delim = strchr(me_config.udpServerAdress, ':');

			if (delim)
			{
				*(delim++) = 0;
				me_config.udpServerPort = atoi(delim);
			}
		}

		// Check if the string looks like an ip address
		if (!(*me_config.udpServerAdress && strz_is_ip(me_config.udpServerAdress, 4)))
		{
			ESP_LOGW(TAG, "UDP server IP address is not specified, working without outgoing messages");

			*me_config.udpServerAdress = 0;
		}
		else if (!me_config.udpServerPort)
		{
			ESP_LOGW(TAG, "Server UDP port not specified, using default %d", DEFAULT_UDP_PORT);
			me_config.udpServerPort = DEFAULT_UDP_PORT;
		}

		if (*me_config.udp_crosslink)
		{
			parseUdpCrossLinks();
		}

		if ((me_state.udplink_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) >= 0)
		{
			ESP_LOGD(TAG,"UDP socket number: %d", me_state.udplink_socket);

			int buff_size=250;
			char buff[buff_size];

			struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
			socklen_t socklen = sizeof(source_addr);

			struct sockaddr_in dest_addr;
			dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
			dest_addr.sin_family = AF_INET;
			dest_addr.sin_port = htons(me_config.udpMyPort);

			int err = bind(me_state.udplink_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
			if (err < 0) {
				ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
			}

			ESP_LOGD(TAG, "UDP listening on port:%d", me_config.udpMyPort);
			
			me_state.UDP_init_res = ESP_OK;
			
			while(1)
			{
				if ((len = recvfrom(me_state.udplink_socket, buff, buff_size - 1, 0,(struct sockaddr *)&source_addr, &socklen)) > 0)
				{
					*(buff + len) = 0;
					//printf("got: '%s'\n", buff);
					execute_links(buff);
				}
			}
		}
		else
		{
			mblog(E, "Failed to create socket for UDP: %d", errno);
		}
	}
	else
	{
		ESP_LOGW(TAG, "Local UDP port is not specified, so UDP is not enabled");
	}

	vTaskDelay(pdMS_TO_TICKS(200));
	vTaskDelete(NULL);
}
int udplink_send(int slot_num, const char * message)
{
    int     result  = 0;

	//printf("udplink_send: stage 1\n");

	if ((me_state.udplink_socket) != -1 && *me_config.udpServerAdress)
	{
		//printf("udplink_send: stage 2\n");

		struct sockaddr_in destAddr = {0};
		destAddr.sin_addr.s_addr = inet_addr(me_config.udpServerAdress);
		destAddr.sin_family = 2;
		destAddr.sin_port = htons(me_config.udpServerPort);
		result = sendto(me_state.udplink_socket, message, strlen(message), 0, (struct sockaddr *)&destAddr, sizeof(destAddr));
	}

    return result;
}

void start_udp_receive_task()
{
	xTaskCreatePinnedToCore(udplink_task, "udplink_task", 1024 * 6, NULL, configMAX_PRIORITIES - 10, NULL,0);
}
