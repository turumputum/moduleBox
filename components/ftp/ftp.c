/*
 * This file is part of the MicroPython ESP32 project, https://github.com/loboris/MicroPython_ESP32_psRAM_LoBo
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 LoBo (https://github.com/loboris)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/*
 * This file is based on 'ftp' from Pycom Limited.
 *
 * Author: LoBo, loboris@gmail.com
 * Copyright (c) 2017, LoBo
 */

#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

//#include "dirent.h"
#include "esp_system.h"
//#include "esp_spi_flash.h"
#include "nvs_flash.h"
//#include "esp_event.h"
#include "esp_log.h"
//#include "esp_netif.h"
#include "esp_wifi.h"

#include "lwip/sockets.h"
//#include "lwip/dns.h"
//#include "lwip/netdb.h"
#include "stateConfig.h"
//#include "mdns.h"

//#include "freertos/semphr.h"

#include "ftp.h"

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

//extern int FTP_TASK_FINISH_BIT;
//extern EventGroupHandle_t xEventTask;

// ================== SHARED DATA ==================

extern configuration me_config;
extern stateStruct me_state;

const char *FTP_TAG = "[FTP]";

const char *TAG_CL = "[FTP]";


static int net_if_num = 0;
static esp_netif_t *net_if[MAX_ACTIVE_INTERFACES];

static const char *MOUNT_POINT = "/sdcard";

static char * ANONUSERNAME1 = "anonymous";
static char * ANONUSERNAME2 = "ak";

static uint8_t ftp_stop = 0;

static uint8_t anon_enabled = 0; 

char ftp_user[FTP_USER_PASS_LEN_MAX + 1];
char ftp_pass[FTP_USER_PASS_LEN_MAX + 1];

// =================================================

/******************************************************************************
 DECLARE PRIVATE DATA
 ******************************************************************************/

// SemaphoreHandle_t shared_var_mutex = NULL;
// #define MUTEX_LOCK 		if (xSemaphoreTake(shared_var_mutex, portMAX_DELAY) == pdTRUE) {
// #define MUTEX_UNLOCK 	xSemaphoreGive(shared_var_mutex);  } 



static CLIENTCOMMON	ftp_common = { -1, -1 };



static const ftp_cmd_t ftp_cmd_table[] = { { "FEAT" }, { "SYST" }, { "CDUP" }, { "CWD"	},
										   { "PWD"	}, { "XPWD" }, { "SIZE" }, { "MDTM" },
										   { "TYPE" }, { "USER" }, { "PASS" }, { "PASV" },
										   { "LIST" }, { "RETR" }, { "STOR" }, { "DELE" },
										   { "RMD"	}, { "MKD"	}, { "RNFR" }, { "RNTO" },
										   { "NOOP" }, { "QUIT" }, { "APPE" }, { "NLST" }, 
										   { "AUTH" }, { "PORT" } };

static char * MSG_250 = "Directory successfully changed.";


static CLIENT cl [ MAX_CLIENTS ];


// ==== PRIVATE FUNCTIONS ===================================================
uint64_t mp_hal_ticks_ms() {
	uint64_t time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
	return time_ms;
}

int network_get_active_interfaces()
{
	ESP_LOGI(FTP_TAG, "network_get_active_interfaces");
	int n_if = 0;

#if 0
	for (int i=0; i<MAX_ACTIVE_INTERFACES; i++) {
		tcpip_if[i] = TCPIP_ADAPTER_IF_MAX;
	}
#endif

	net_if[0] = esp_netif_get_handle_from_ifkey("ETH_DEF");
	n_if += 1;

	//if (wifi_is_started())
	{
		wifi_mode_t mode;
		esp_err_t ret = esp_wifi_get_mode(&mode);
		if (ret == ESP_OK) {
			if (mode == WIFI_MODE_STA) {
				n_if += 1;
				//tcpip_if[0] = TCPIP_ADAPTER_IF_STA;
				net_if[n_if - 1] = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
			}
			else if (mode == WIFI_MODE_AP) {
				n_if += 1;
				//tcpip_if[0] = TCPIP_ADAPTER_IF_AP;
				net_if[n_if - 1] = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
			}
			else if (mode == WIFI_MODE_APSTA) {
				n_if += 2;
				//tcpip_if[0] = TCPIP_ADAPTER_IF_STA;
				net_if[n_if - 2] = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
				//tcpip_if[1] = TCPIP_ADAPTER_IF_AP;
				net_if[n_if - 1] = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
			}
			else {
				ESP_LOGE(FTP_TAG, "esp_wifi_get_mode fail");
			}
		}
	}

	return n_if;
}
int get_listen_addr(int32_t _sd, uint32_t *ip_addr)
{
	int  result = -1;

	socklen_t  in_addrSize;
	struct sockaddr_in ifaceAddr;

	in_addrSize = sizeof(struct sockaddr_in);

	if ((result = getsockname(_sd, (struct sockaddr *)&ifaceAddr, (socklen_t *)&in_addrSize)) == 0)
	{
		*ip_addr = ifaceAddr.sin_addr.s_addr;
	}

	return result;
}

//--------------------------------
static void stoupper (char *str) {
	while (str && *str != '\0') {
		*str = (char)toupper((int)(*str));
		str++;
	}
}

// ==== File functions =========================================

//--------------------------------------------------------------
static bool ftp_open_file (PCLIENT cl, const char *path, const char *mode) {
	ESP_LOGI(TAG_CL, "ftp_open_file: path=[%s]", path);
	char fullname[128];
	strcpy(fullname, MOUNT_POINT);
	strcat(fullname, path);
	ESP_LOGI(TAG_CL, "ftp_open_file: fullname=[%s]", fullname);
	//cl->ftp_data.fp = fopen(path, mode);
	cl->ftp_data.fp = fopen(fullname, mode);
	if (cl->ftp_data.fp == NULL) {
		ESP_LOGE(TAG_CL, "ftp_open_file: open fail [%s]", fullname);
		return false;
	}
	cl->ftp_data.e_open = E_FTP_FILE_OPEN;
	return true;
}

//--------------------------------------
static void ftp_close_files_dir (PCLIENT cl) {
	if (cl->ftp_data.e_open == E_FTP_FILE_OPEN) {
		fclose(cl->ftp_data.fp);
		cl->ftp_data.fp = NULL;
	}
	else if (cl->ftp_data.e_open == E_FTP_DIR_OPEN) {
		closedir(cl->ftp_data.dp);
		cl->ftp_data.dp = NULL;
	}
	cl->ftp_data.e_open = E_FTP_NOTHING_OPEN;
}

//------------------------------------------------
static void ftp_close_filesystem_on_error (PCLIENT cl) {
	ftp_close_files_dir(cl);
	if (cl->ftp_data.fp) {
		fclose(cl->ftp_data.fp);
		cl->ftp_data.fp = NULL;
	}
	if (cl->ftp_data.dp) {
		closedir(cl->ftp_data.dp);
		cl->ftp_data.dp = NULL;
	}
}

//---------------------------------------------------------------------------------------------
static ftp_result_t ftp_read_file (PCLIENT cl, char *filebuf, uint32_t desiredsize, uint32_t *actualsize) 
{
	ESP_LOGI(TAG_CL, "ftp_read_file: filebuf=[%p] desiredsize=[%u]", filebuf, (unsigned)desiredsize);

	ftp_result_t result = E_FTP_RESULT_CONTINUE;
	//MUTEX_LOCK
	*actualsize = fread(filebuf, 1, desiredsize, cl->ftp_data.fp);
	//MUTEX_UNLOCK
	if (*actualsize == 0) {
		if (feof(cl->ftp_data.fp))
			result = E_FTP_RESULT_OK;
		else
			result = E_FTP_RESULT_FAILED;
		ftp_close_files_dir(cl);
	} else if (*actualsize < desiredsize) {
		ftp_close_files_dir(cl);
		result = E_FTP_RESULT_OK;
	}
	return result;
}

//-----------------------------------------------------------------
static ftp_result_t ftp_write_file (PCLIENT cl, char *filebuf, uint32_t size) {
	ftp_result_t result = E_FTP_RESULT_FAILED;
	uint32_t actualsize = fwrite(filebuf, 1, size, cl->ftp_data.fp);
	if (actualsize == size) {
		result = E_FTP_RESULT_OK;
	} else {
		ftp_close_files_dir(cl);
	}
	return result;
}

static void normalize_path(char * path)
{
	char *on;

	if (!strcmp("/.", path))
	{
		*(path + 1) = 0;
	}
	else if ((on = strstr(path, "/./")) != 0)
	{
		strcpy(on, on + 2);
	}

	ESP_LOGI(FTP_TAG, "normal path: %s", path);
}

//---------------------------------------------------------------
static ftp_result_t ftp_open_dir_for_listing (PCLIENT cl, const char *path) {
	if (cl->ftp_data.dp) {
		closedir(cl->ftp_data.dp);
		cl->ftp_data.dp = NULL;
	}
	ESP_LOGI(TAG_CL, "ftp_open_dir_for_listing path=[%s] MOUNT_POINT=[%s], switches: [%s]", path, MOUNT_POINT, cl->switchesBuff);
	char fullname[128];
	strcpy(fullname, MOUNT_POINT);
	strcat(fullname, path);
	ESP_LOGI(TAG_CL, "ftp_open_dir_for_listing: %s", fullname);
	cl->ftp_data.dp = opendir(fullname);  // Open the directory
	if (cl->ftp_data.dp == NULL) {
		return E_FTP_RESULT_FAILED;
	}
	cl->ftp_data.e_open = E_FTP_DIR_OPEN;
	cl->ftp_data.listroot = false;
	return E_FTP_RESULT_CONTINUE;
}

static int extract_client_port_params(struct sockaddr_in * 	 port, char * ftp_scratch_buffer)
{
	int 		cnt			= 0;
	int 		num			= 0;
	uint32_t	addr 		= 0;
	char * 		on			= ftp_scratch_buffer;
	char * 		end;

	if (strlen(on) > 0)
	{
		port->sin_family 		= AF_INET;
		port->sin_addr.s_addr = 0;
		port->sin_len 		= sizeof(struct sockaddr_in);
		port->sin_port		= 0;

		while (on)
		{
			end = strchr(on, ','); 
			if (end != 0)
			{
				*end = 0;
				end++;
			}
			switch (cnt)
			{
				case 0:
				case 1:
				case 2:
				case 3:
					addr <<= 8;
					addr |= atol(on); 
					break;
				case 4:
					num = atol(on) * 256;
					break;
				case 5:
					num += atol(on);
					break;

				default:
					break;
			}

			on = end;
			cnt++;
		}

		port->sin_addr.s_addr 	= htonl(addr);
		port->sin_port 			= htons(num);

		ESP_LOGI(TAG_CL, "CMD: client port: %d.%d.%d.%d port %d", 
					(int)((addr >> 24) & 0xff), 
					(int)((addr >> 16) & 0xff), 
					(int)((addr >> 8) & 0xff), 
					(int)((addr >> 0) & 0xff), 
					num );
	}


	return cnt;
}
#if 0
//---------------------------------------------------------------------------
static int ftp_get_eplf_drive (char *dest, uint32_t destsize, char *name) {
	char *type = "d";
	struct tm *tm_info;
	time_t seconds;
	time(&seconds); // get the time from the RTC
	tm_info = gmtime(&seconds);
	char str_time[64];
	strftime(str_time, 63, "%b %d %Y", tm_info);

	return snprintf(dest, destsize, "%srw-rw-rw-   1 root  root %9u %s %s\r\n", type, 0, str_time, name);
}
#endif

//---------------------------------------------------------------------------------
static int ftp_get_eplf_item (char *dest, uint32_t destsize, struct dirent *de) 
{
	int result = 0;
	char *type  = (de->d_type & DT_DIR) ? "d" : "-";
	char *type2 = (de->d_type & DT_DIR) ? "x" : "-";

	// Get full file path needed for stat function
	char fullname[128];
	strcpy(fullname, MOUNT_POINT);
	strcat(fullname, cl->ftp_path);
	//strcpy(fullname, cl->ftp_path);

	if (fullname[strlen(fullname)-1] != '/') strcat(fullname, "/");
	strcat(fullname, de->d_name);

	struct stat buf;
	int res = stat(fullname, &buf);
	//ESP_LOGI(TAG_CL, "ftp_get_eplf_item res=%d buf.st_size=%ld", res, buf.st_size);
	if (res < 0) {
		buf.st_size = 0;
		buf.st_mtime = 946684800; // Jan 1, 2000
	}

	if (de->d_type & DT_DIR)
		buf.st_size = 4096;

	char str_time[64];
	struct tm *tm_info;
	time_t now;
	if (time(&now) < 0) now = 946684800;	// get the current time from the RTC
	tm_info = localtime(&buf.st_mtime);		// get broken-down file time

	// if file is older than 180 days show dat,month,year else show month, day and time
	if ((buf.st_mtime + FTP_UNIX_SECONDS_180_DAYS) < now) strftime(str_time, 127, "%b %d %Y", tm_info);
	else strftime(str_time, 63, "%b %d %H:%M", tm_info);

	if (cl->ftp_nlist)
		result = snprintf(0, 0, "%s\r\n", de->d_name);
	else 
		result = snprintf(0, 0, "%srw%srw%srw%s    2 1000     1000 %12"PRIu32" %s %s\r\n", type, type2, type2, type2, (uint32_t)buf.st_size, str_time, de->d_name);

	if (result >= destsize)
	{
		int offset 			= dest - (char*)cl->ftp_data.dBuffer;
		int delta 			= result + 64;
		long new_size 		= cl->ftp_buff_size + delta + 1;

		ESP_LOGW(TAG_CL, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@ Buffer too small, reallocating [%d > %"PRIi32"]", cl->ftp_buff_size, new_size);

		char * new_buffer 	= realloc(cl->ftp_data.dBuffer, new_size);

		if (new_buffer) 
		{
			cl->ftp_data.dBuffer 	= (uint8_t*)new_buffer;
			cl->ftp_buff_size 		= new_size - 1;
			dest 					= (char*)cl->ftp_data.dBuffer + offset;
			destsize 			   += delta;
		}
		else 
		{
			ESP_LOGE(TAG_CL, "Buffer reallocation ERROR");
			result = 0;
		}
	}

	if (result)
	{
		if (cl->ftp_nlist) 
			result = snprintf(dest, destsize, "%s\r\n", de->d_name);
		else 
			result = snprintf(dest, destsize, "%srw%srw%srw%s    2 1000     1000 %12"PRIu32" %s %s\r\n", type, type2, type2, type2, (uint32_t)buf.st_size, str_time, de->d_name);
	}

	return result;
}
//--------------------------------------------------------------------------------------
static ftp_result_t ftp_list_dir(PCLIENT cl, uint32_t *listsize) {
	uint next = 0;
	uint listcount = 0;
	ftp_result_t result = E_FTP_RESULT_CONTINUE;
	struct dirent *de;

	// read up to 8 directory items
	while (listcount < 8)
	{
		de = readdir(cl->ftp_data.dp);															// Read a directory item
		//ESP_LOGI(TAG_CL, "readdir de=%p", de);
		if (de == NULL) {
			result = E_FTP_RESULT_OK;
			break;																			// Break on error or end of dp
		}

		if (!cl->listFlagAll)
		{
			if (de->d_name[0] == '.' && de->d_name[1] == 0) continue;							// Ignore . entry
			if (de->d_name[0] == '.' && de->d_name[1] == '.' && de->d_name[2] == 0) continue;	// Ignore .. entry
		}

		// add the entry to the list
		//ESP_LOGI(TAG_CL, "FTP%.2d: Add to dir list %p(%d): %s", cl->num, list, (int)maxlistsize, de->d_name);
		next += ftp_get_eplf_item(((char *)cl->ftp_data.dBuffer + next), (cl->ftp_buff_size - next), de);
		listcount++;
	}
	if (result == E_FTP_RESULT_OK) {
		ftp_close_files_dir(cl);
	}
	*listsize = next;
	return result;
}

// ==== Socket functions ==============================================================

//------------------------------------
static void ftp_close_cmd_data(PCLIENT cl) {
	if (cl->ftp_data.c_sd != -1)
	{
		closesocket(cl->ftp_data.c_sd);
		cl->ftp_data.c_sd  = -1;
	}

	if (cl->ftp_data.d_sd != -1)
	{
		closesocket(cl->ftp_data.d_sd);
		cl->ftp_data.d_sd  = -1;
	}

	ftp_close_filesystem_on_error (cl);

	cl->inUse = false;
	cl->resetTrigger = true;
}

//----------------------------
static void _ftp_reset(PCLIENT cl) {
	// close all connections and start all over again
	ESP_LOGW(TAG_CL, "FTP RESET");
	ftp_close_cmd_data(cl);

	cl->ftp_data.e_open = E_FTP_NOTHING_OPEN;
	cl->ftp_data.state = E_FTP_STE_START;
	cl->ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
}

//-------------------------------------------------------------------------------------
static bool ftp_create_listening_socket (int32_t *sd, uint32_t port, uint8_t backlog) {
	struct sockaddr_in sServerAddress;
	int32_t _sd;
	int32_t result;

	// open a socket for ftp data listen
	*sd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	_sd = *sd;

	if (_sd > 0) {
		// enable non-blocking mode
		uint32_t option = fcntl(_sd, F_GETFL, 0);
		option |= O_NONBLOCK;
		fcntl(_sd, F_SETFL, option);

		// enable address reusing
		option = 1;
		result = setsockopt(_sd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

		// bind the socket to a port number
		sServerAddress.sin_family = AF_INET;
		sServerAddress.sin_addr.s_addr = INADDR_ANY;
		sServerAddress.sin_len = sizeof(sServerAddress);
		sServerAddress.sin_port = htons(port);

		result |= bind(_sd, (const struct sockaddr *)&sServerAddress, sizeof(sServerAddress));

		// start listening
		result |= listen (_sd, backlog);

		if (!result) {
			return true;
		}
		closesocket(*sd);
	}
	return false;
}

//--------------------------------------------------------------------------------------------
static ftp_result_t ftp_wait_for_connection (PCLIENT cl, int32_t l_sd, int32_t *n_sd, uint32_t *ip_addr) {
	struct sockaddr_in	sClientAddress;
	socklen_t  in_addrSize;

	// accepts a connection from a TCP client, if there is any, otherwise returns EAGAIN
	//ESP_LOGI(TAG_CL, "call accept");
	*n_sd = accept(l_sd, (struct sockaddr *)&sClientAddress, (socklen_t *)&in_addrSize);
	int32_t _sd = *n_sd;
	if (_sd < 0) {
		if (errno == EAGAIN) {
			return E_FTP_RESULT_CONTINUE;
		}
		// error
		_ftp_reset(cl);
		return E_FTP_RESULT_FAILED;
	}

	if (ip_addr) {
		// check on which network interface the client was connected and save the IP address
		//tcpip_adapter_ip_info_t ip_info = {0};
		esp_netif_ip_info_t ip_info;

		if (net_if_num > 0) {
			struct sockaddr_in clientAddr;
			in_addrSize = sizeof(struct sockaddr_in);
			getpeername(_sd, (struct sockaddr *)&clientAddr, (socklen_t *)&in_addrSize);
			ESP_LOGI(TAG_CL, "Client IP: 0x%08"PRIx32, clientAddr.sin_addr.s_addr);
			*ip_addr = 0;
			for (int i=0; i<net_if_num; i++) {
				//tcpip_adapter_get_ip_info(tcpip_if[i], &ip_info);
				esp_netif_get_ip_info(net_if[i], &ip_info);
				ESP_LOGI(TAG_CL, "Adapter: 0x%08"PRIx32", 0x%08"PRIx32, ip_info.ip.addr, ip_info.netmask.addr);
				if ((ip_info.ip.addr & ip_info.netmask.addr) == (ip_info.netmask.addr & clientAddr.sin_addr.s_addr)) {
					*ip_addr = ip_info.ip.addr;
					//ESP_LOGI(TAG_CL, "Client connected on interface %d", net_if[i]);
					char name[8];
					esp_netif_get_netif_impl_name(net_if[i], name);
					ESP_LOGI(TAG_CL, "Client connected on interface %s", name);
					break;
				}
			}
			if (*ip_addr == 0) {
				ESP_LOGE(TAG_CL, "No IP address detected (?!)");
			}
		}
		else {
			ESP_LOGE(TAG_CL, "No active interface (?!)");
		}
	}

	// enable non-blocking mode if not data channel connection
	uint32_t option = fcntl(_sd, F_GETFL, 0);
	if (l_sd != cl->common->ld_sd) option |= O_NONBLOCK;
	fcntl(_sd, F_SETFL, option);

	// client connected, so go on
	return E_FTP_RESULT_OK;
}

//-----------------------------------------------------------
static void ftp_send_reply (PCLIENT cl, uint32_t status, char *message) {
	if (!message) {
		message = "";
	}
	snprintf((char *)cl->ftp_cmd_buffer, 4, "%"PRIu32, status);
	strcat ((char *)cl->ftp_cmd_buffer, " ");
	strcat ((char *)cl->ftp_cmd_buffer, message);
	strcat ((char *)cl->ftp_cmd_buffer, "\r\n");

	int32_t timeout = 200;
	ftp_result_t result;
	//uint32_t size = strlen((char *)cl->ftp_cmd_buffer);
	size_t size = strlen((char *)cl->ftp_cmd_buffer);

	ESP_LOGI(TAG_CL, "Send reply: [%.*s]", size-2, cl->ftp_cmd_buffer);
	vTaskDelay(1);

	while (1) {
		result = send(cl->ftp_data.c_sd, cl->ftp_cmd_buffer, size, 0);
		if (result == size) {
			if (status == 221) {
				cl->ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
				ftp_close_cmd_data(cl);
			}
			else if (status == 426 || status == 451 || status == 550) {
				closesocket(cl->ftp_data.d_sd);
				cl->ftp_data.d_sd = -1;
				ftp_close_filesystem_on_error(cl);
			}
			vTaskDelay(1);
			ESP_LOGI(TAG_CL, "Send reply: OK (%u)", size);
			break;
		}
		else {
			vTaskDelay(1);
			if ((timeout <= 0) || (errno != EAGAIN)) {
				// error
				_ftp_reset(cl);
				ESP_LOGW(TAG_CL, "Error sending command reply.");
				break;
			}
		}
		timeout -= portTICK_PERIOD_MS;
	}
}

static char dummy_root[] = { "drwxr-xr-x    3 0        0            4096 Jan 01 00:30 .\r\n"
                             "drwxr-xr-x    3 0        0            4096 Jan 01 00:30 ..\r\n" } ;


//------------------------------------------
static void ftp_send_list(PCLIENT cl, uint32_t datasize)
{
	int32_t timeout = 200;
	ftp_result_t result;

	ESP_LOGI(TAG_CL, "Send list data: (%"PRIu32")", datasize);
	vTaskDelay(1);

	// if (cl->listFlagAll)
	// {
	// 	send(cl->ftp_data.d_sd, dummy_root, sizeof(dummy_root) - 1, 0);
	// }

	while (1) {
		result = send(cl->ftp_data.d_sd, cl->ftp_data.dBuffer, datasize, 0);
		if (result == datasize) {
			vTaskDelay(1);
			ESP_LOGI(TAG_CL, "Send OK");
			break;
		}
		else {
			vTaskDelay(1);
			if ((timeout <= 0) || (errno != EAGAIN)) {
				// error
				_ftp_reset(cl);
				ESP_LOGW(TAG_CL, "Error sending list data.");
				break;
			}
		}
		timeout -= portTICK_PERIOD_MS;
	}
}

//-----------------------------------------------
static void ftp_send_file_data(PCLIENT cl, uint32_t datasize)
{
	ftp_result_t result;
	uint32_t timeout = 200;

	ESP_LOGI(TAG_CL, "Send file data: (%"PRIu32")", datasize);
	vTaskDelay(1);

	while (1) {
		result = send(cl->ftp_data.d_sd, cl->ftp_data.dBuffer, datasize, 0);
		if (result == datasize) {
			vTaskDelay(1);
			ESP_LOGI(TAG_CL, "Send OK");
			break;
		}
		else {
			vTaskDelay(1);
			if ((timeout <= 0) || (errno != EAGAIN)) {
				// error
				_ftp_reset(cl);
				ESP_LOGW(TAG_CL, "Error sending file data.");
				break;
			}
		}
		timeout -= portTICK_PERIOD_MS;
	}
}

//------------------------------------------------------------------------------------------------
static ftp_result_t ftp_recv_non_blocking (int32_t sd, void *buff, int32_t Maxlen, int32_t *rxLen)
{
	if (sd < 0) return E_FTP_RESULT_FAILED;

	*rxLen = recv(sd, buff, Maxlen, 0);
	if (*rxLen > 0) return E_FTP_RESULT_OK;
	else if (errno != EAGAIN) return E_FTP_RESULT_FAILED;

	return E_FTP_RESULT_CONTINUE;
}

// ==== Directory functions =======================

//-----------------------------------
#if 0
static void ftp_fix_path(char *pwd) {
	ESP_LOGI(TAG_CL, "ftp_fix_path: pwd=[%s]", pwd);
	char ph_path[128];
	uint len = strlen(pwd);

	if (len == 0) {
		strcpy (pwd, "/");
	}
	else if ((len > 1) && (pwd[len-1] == '/')) pwd[len-1] = '\0';

	// Convert to physical path
	if (strstr(pwd, VFS_NATIVE_INTERNAL_MP) == pwd) {
		ESP_LOGI(TAG_CL, "ftp_fix_path: VFS_NATIVE_INTERNAL_MP=[%s]", VFS_NATIVE_INTERNAL_MP);
		sprintf(ph_path, "%s%s", VFS_NATIVE_MOUNT_POINT, pwd+strlen(VFS_NATIVE_INTERNAL_MP));
		if (strcmp(ph_path, VFS_NATIVE_MOUNT_POINT) == 0) strcat(ph_path, "/");
		ESP_LOGI(TAG_CL, "ftp_fix_path: ph_path=[%s]", ph_path);
		strcpy(pwd, ph_path);
	}
	else if (strstr(pwd, VFS_NATIVE_EXTERNAL_MP) == pwd) {
		ESP_LOGI(TAG_CL, "ftp_fix_path: VFS_NATIVE_EXTERNAL_MP=[%s]", VFS_NATIVE_EXTERNAL_MP);
		sprintf(ph_path, "%s%s", VFS_NATIVE_SDCARD_MOUNT_POINT, pwd+strlen(VFS_NATIVE_EXTERNAL_MP));
		if (strcmp(ph_path, VFS_NATIVE_SDCARD_MOUNT_POINT) == 0) strcat(ph_path, "/");
		ESP_LOGI(TAG_CL, "ftp_fix_path: ph_path=[%s]", ph_path);
		strcpy(pwd, ph_path);
	}
}
#endif


/*
 * Add directory or file name to the current path
 * Initially, pwd is set to "/" (file system root)
 * There are two possible entries in root:
 *	 "flash"	which can be fatfs or spiffs
 *	 "sd"		sd card
 * flash and sd entries have to be translated to their VFS names
 * trailing '/' is required for flash&sd root entries (translated)
 */
//-------------------------------------------------
static void ftp_open_child (char *pwd, char *dir) {
	ESP_LOGI(TAG_CL, "open_child: %s + %s", pwd, dir);
	if (strlen(dir) > 0) {
		if (dir[0] == '/') {
			// ** absolute path
			strcpy(pwd, dir);
		}
		else {
			// ** relative path
			// add trailing '/' if needed
			if ((strlen(pwd) > 1) && (pwd[strlen(pwd)-1] != '/') && (dir[0] != '/')) strcat(pwd, "/");
			// append directory/file name
			strcat(pwd, dir);
		}
#if 0
		ftp_fix_path(pwd);
#endif
	}
	ESP_LOGI(TAG_CL, "open_child, New pwd: %s", pwd);
}

// Return to parent directory
//---------------------------------------
static void ftp_close_child (char *pwd) {
	ESP_LOGI(TAG_CL, "close_child: %s", pwd);
	uint len = strlen(pwd);
	if (pwd[len-1] == '/') {
		pwd[len-1] = '\0';
		len--;
		if ((len == 0) || (strcmp(pwd, VFS_NATIVE_MOUNT_POINT) == 0) || (strcmp(pwd, VFS_NATIVE_SDCARD_MOUNT_POINT) == 0)) {
			strcpy(pwd, "/");
		}
	}
	else {
		while (len) {
			if (pwd[len-1] == '/') {
				pwd[len-1] = '\0';
				len--;
				break;
			}
			len--;
		}

		if (len == 0) {
			strcpy (pwd, "/");
		}
		else if ((strcmp(pwd, VFS_NATIVE_MOUNT_POINT) == 0) || (strcmp(pwd, VFS_NATIVE_SDCARD_MOUNT_POINT) == 0)) {
			strcat(pwd, "/");
		}
	}
	ESP_LOGI(TAG_CL, "close_child, New pwd: %s", pwd);
}

// Remove file name from path
//-----------------------------------------------------------
static void remove_fname_from_path (char *pwd, char *fname) {
	ESP_LOGI(TAG_CL, "remove_fname_from_path: %s - %s", pwd, fname);
	if (strlen(fname) == 0) return;
	char *xpwd = strstr(pwd, fname);
	if (xpwd == NULL) return;

	xpwd[0] = '\0';

#if 0
	ftp_fix_path(pwd);
#endif
	ESP_LOGI(TAG_CL, "remove_fname_from_path: New pwd: %s", pwd);
}

// ==== Param functions =================================================

//------------------------------------------------------------------------------------------
static void ftp_pop_param(char **str, char *param, bool stop_on_space, bool stop_on_newline, bool enaSwitches)
{
	char lastc = '\0';
	char *switches = &cl->switchesBuff[0];
	int switchedMode = 0;

	while (**str != '\0') {
		if (stop_on_space && (**str == ' ')) break;
		if ((**str == '\r') || (**str == '\n')) {
			if (!stop_on_newline) {
				(*str)++;
				continue;
			}
			else break;
		}
		if ((**str == '/') && (lastc == '/')) {
			(*str)++;
			continue;
		}

		if (enaSwitches && (**str == '-') && ((lastc == 0) || (lastc == ' '))) {
			switchedMode = 1;
			(*str)++;
			continue;
		}

		if (switchedMode)
		{
			if (**str != ' ')
			{
				lastc = **str;
				*switches++ = **str;
			}
			else
				switchedMode = 0;

			(*str)++;
		}
		else
		{
			lastc = **str;
			*param++ = **str;
			(*str)++;
		}
	}
	*param = '\0';
	*switches = '\0';
}
static void parse_list_params()
{
	char *switches = &cl->switchesBuff[0];
	char *on;

	cl->listFlagAll = false;
	cl->listFlagLong = true;

	for (on = switches; *on; on++)
	{
		switch (*on)
		{
			case 'a':
				cl->listFlagAll = true;
				break;
			
			default:
				break;
		}
	}
}


//--------------------------------------------------
static ftp_cmd_index_t ftp_pop_command(char **str) {
	char _cmd[FTP_CMD_SIZE_MAX];
	ftp_pop_param (str, _cmd, true, true, false);
	stoupper (_cmd);
	for (ftp_cmd_index_t i = 0; i < E_FTP_NUM_FTP_CMDS; i++) {
		if (!strcmp (_cmd, ftp_cmd_table[i].cmd)) {
			// move one step further to skip the space
			(*str)++;
			return i;
		}
	}
	return E_FTP_CMD_NOT_SUPPORTED;
}

// Get file name from parameter and append to cl->ftp_path
//-------------------------------------------------------
static void ftp_get_param_and_open_child(PCLIENT cl, char **bufptr) {
	ftp_pop_param(bufptr, cl->ftp_scratch_buffer, false, false, true);
	normalize_path(cl->ftp_scratch_buffer);
	ftp_open_child(cl->ftp_path, cl->ftp_scratch_buffer);
	cl->ftp_data.closechild = true;
}

// ==== Ftp command processing =====

//----------------------------------
static void ftp_process_cmd (PCLIENT cl) {
	int32_t len;
	char *bufptr = (char *)cl->ftp_cmd_buffer;
	ftp_result_t result;
	struct stat buf;
	int res;
	memset(bufptr, 0, FTP_MAX_PARAM_SIZE + FTP_CMD_SIZE_MAX);
	cl->ftp_data.closechild = false;

	// use the reply buffer to receive new commands
	result = ftp_recv_non_blocking(cl->ftp_data.c_sd, cl->ftp_cmd_buffer, FTP_MAX_PARAM_SIZE + FTP_CMD_SIZE_MAX, &len);
	if (result == E_FTP_RESULT_OK) {
		cl->ftp_cmd_buffer[len] = '\0';
		ESP_LOGI(TAG_CL, "GOT CMD: %s", cl->ftp_cmd_buffer);
		// bufptr is moved as commands are being popped
		ftp_cmd_index_t cmd = ftp_pop_command(&bufptr);
		if (!cl->ftp_data.loggin.passvalid &&
				((cmd != E_FTP_CMD_USER) && (cmd != E_FTP_CMD_PASS) && (cmd != E_FTP_CMD_QUIT) && (cmd != E_FTP_CMD_FEAT) && (cmd != E_FTP_CMD_AUTH))) {
			ftp_send_reply(cl, 332, NULL);
			return;
		}
		if ((cmd >= 0) && (cmd < E_FTP_NUM_FTP_CMDS)) {
			ESP_LOGI(TAG_CL, "CMD: %s", ftp_cmd_table[cmd].cmd);
		}
		else {
			ESP_LOGI(TAG_CL, "CMD: %d", cmd);
		}
		char fullname[128];
		char fullname2[128];
		strcpy(fullname, MOUNT_POINT);
		strcpy(fullname2, MOUNT_POINT);

		switch (cmd) {
		case E_FTP_CMD_FEAT:
			ftp_send_reply(cl, 502, "no-features");
			break;
		case E_FTP_CMD_AUTH:
			ftp_send_reply(cl, 504, "not-supported");
			break;
		case E_FTP_CMD_SYST:
			ftp_send_reply(cl, 215, "UNIX Type: L8");
			break;
		case E_FTP_CMD_CDUP:
			ftp_close_child(cl->ftp_path);
			ftp_send_reply(cl, 250, MSG_250);
			break;
		case E_FTP_CMD_PORT:
			{
				struct sockaddr_in 	portDef;
				ftp_pop_param (&bufptr, cl->ftp_scratch_buffer, false, false, false);

				if (extract_client_port_params(&portDef, cl->ftp_scratch_buffer) > 0) 
				{
					if (cl->ftp_data.d_sd >= 0)
					{
						closesocket(cl->ftp_data.d_sd);
						cl->ftp_data.d_sd  = -1;
					}

	// @@@
					cl->ftp_data.d_sd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

					if (cl->ftp_data.d_sd != -1)
					{
						if (connect(cl->ftp_data.d_sd, (struct sockaddr_in *)&portDef, sizeof(portDef)) == 0)
						{
							ftp_send_reply(cl, 200, "PORT command successful.");
						}
						else
						{
							ftp_send_reply(cl, 530, "Connection failed");
						}
					}
					else
						ftp_send_reply(cl, 530, "Socket failed");
				}
				else
					ftp_send_reply(cl, 501, "Params parsing failed");
			}
			break;
		case E_FTP_CMD_CWD:
			ftp_pop_param (&bufptr, cl->ftp_scratch_buffer, false, false, false);
			normalize_path(cl->ftp_scratch_buffer);

			if (strlen(cl->ftp_scratch_buffer) > 0) {
				if ((cl->ftp_scratch_buffer[0] == '.') && (cl->ftp_scratch_buffer[1] == '\0')) {
					cl->ftp_data.dp = NULL;
					ftp_send_reply(cl, 250, MSG_250);
					break;
				}
				if ((cl->ftp_scratch_buffer[0] == '.') && (cl->ftp_scratch_buffer[1] == '.') && (cl->ftp_scratch_buffer[2] == '\0')) {
					ftp_close_child (cl->ftp_path);
					ftp_send_reply(cl, 250, MSG_250);
					break;
				}
				else ftp_open_child (cl->ftp_path, cl->ftp_scratch_buffer);
			}

			if ((cl->ftp_path[0] == '/') && (cl->ftp_path[1] == '\0')) {
				cl->ftp_data.dp = NULL;
				ftp_send_reply(cl, 250, MSG_250);
			}
			else {
				strcat(fullname, cl->ftp_path);
				ESP_LOGI(TAG_CL, "E_FTP_CMD_CWD fullname=[%s]", fullname);
				//cl->ftp_data.dp = opendir(cl->ftp_path);
				cl->ftp_data.dp = opendir(fullname);
				if (cl->ftp_data.dp != NULL) {
					closedir(cl->ftp_data.dp);
					cl->ftp_data.dp = NULL;
					ftp_send_reply(cl, 250, MSG_250);
				}
				else {
					ftp_close_child (cl->ftp_path);
					ftp_send_reply(cl, 550, NULL);
				}
			}
			break;
		case E_FTP_CMD_PWD:
		case E_FTP_CMD_XPWD:
			{
				char lpath[128];
#if 0
				if (strstr(cl->ftp_path, VFS_NATIVE_MOUNT_POINT) == cl->ftp_path) {
					sprintf(lpath, "%s%s", VFS_NATIVE_INTERNAL_MP, cl->ftp_path+strlen(VFS_NATIVE_MOUNT_POINT));
				}
				else if (strstr(cl->ftp_path, VFS_NATIVE_SDCARD_MOUNT_POINT) == cl->ftp_path) {
					sprintf(lpath, "%s%s", VFS_NATIVE_EXTERNAL_MP, cl->ftp_path+strlen(VFS_NATIVE_SDCARD_MOUNT_POINT));
				}
				else strcpy(lpath,cl->ftp_path);
#endif
				
				sprintf(lpath, "\"%s\" is the current directory", cl->ftp_path);
				//strcpy(lpath,cl->ftp_path);

				ftp_send_reply(cl, 257, lpath);
			}
			break;
		case E_FTP_CMD_SIZE:
			{
				ftp_get_param_and_open_child (cl, &bufptr);
				strcat(fullname, cl->ftp_path);
				ESP_LOGI(TAG_CL, "E_FTP_CMD_SIZE fullname=[%s]", fullname);
				//int res = stat(cl->ftp_path, &buf);
				int res = stat(fullname, &buf);
				if (res == 0) {
					// send the file size
					snprintf((char *)cl->ftp_data.dBuffer, cl->ftp_buff_size, "%"PRIu32, (uint32_t)buf.st_size);
					ftp_send_reply(cl, 213, (char *)cl->ftp_data.dBuffer);
				} else {
					ftp_send_reply(cl, 550, NULL);
				}
			}
			break;
		case E_FTP_CMD_MDTM:
			ftp_get_param_and_open_child (cl, &bufptr);
			strcat(fullname, cl->ftp_path);
			ESP_LOGI(TAG_CL, "E_FTP_CMD_MDTM fullname=[%s]", fullname);
			//res = stat(cl->ftp_path, &buf);
			res = stat(fullname, &buf);
			if (res == 0) {
				// send the file modification time
				//snprintf((char *)cl->ftp_data.dBuffer, cl->ftp_buff_size, "%u", (uint32_t)buf.st_mtime);
				//snprintf((char *)cl->ftp_data.dBuffer, cl->ftp_buff_size, "20210212010203");
				//time_t time = buf.st_mtime + (CONFIG_LOCAL_TIMEZONE*60*60);
				time_t time = buf.st_mtime;
				struct tm *ptm = localtime(&time);
				//char buf[128];
				//strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", ptm);
				strftime((char *)cl->ftp_data.dBuffer, cl->ftp_buff_size, "%Y%m%d%H%M%S", ptm);
				ESP_LOGI(TAG_CL, "E_FTP_CMD_MDTM cl->ftp_data.dBuffer=[%s]", cl->ftp_data.dBuffer);
				ftp_send_reply(cl, 213, (char *)cl->ftp_data.dBuffer);
			} else {
				ftp_send_reply(cl, 550, NULL);
			}
			break;

		case E_FTP_CMD_TYPE:
			ftp_send_reply(cl, 200, "Switching to mode.");
			break;

		case E_FTP_CMD_USER:
			ftp_pop_param (&bufptr, cl->ftp_scratch_buffer, true, true, false);
			if  (anon_enabled && (!memcmp(cl->ftp_scratch_buffer, ANONUSERNAME1, strlen(ANONUSERNAME1))  || 
			                      !memcmp(cl->ftp_scratch_buffer, ANONUSERNAME2, strlen(ANONUSERNAME2))  )   )
			{
				cl->ftp_data.loggin.useranon  = true;
				cl->ftp_data.loggin.uservalid = true;
				cl->ftp_data.loggin.passvalid = true;				
				//ftp_send_reply(cl, 331, "Please specify the e-mail.");
				ftp_send_reply(cl, 230, "Login successful.");
			}
			else if (!memcmp(cl->ftp_scratch_buffer, ftp_user, MAX(strlen(cl->ftp_scratch_buffer), strlen(ftp_user))))
			{
				cl->ftp_data.loggin.useranon  = false;
				cl->ftp_data.loggin.uservalid = true && (strlen(ftp_user) == strlen(cl->ftp_scratch_buffer));
				ftp_send_reply(cl, 331, "Please specify the password.");
			}
			break;
		case E_FTP_CMD_PASS:
			ftp_pop_param (&bufptr, cl->ftp_scratch_buffer, true, true, false);
			if (cl->ftp_data.loggin.useranon)
			{
				 if ( (strlen(cl->ftp_scratch_buffer) >= 1) && 
				      (strchr(cl->ftp_scratch_buffer, '@'))	)
				{
					cl->ftp_data.loggin.passvalid = true;
					ftp_send_reply(cl, 230, "Login successful.");
					break;
				}
			}
			else if (!memcmp(cl->ftp_scratch_buffer, ftp_pass, MAX(strlen(cl->ftp_scratch_buffer), strlen(ftp_pass))) &&
					cl->ftp_data.loggin.uservalid) {
				cl->ftp_data.loggin.passvalid = true && (strlen(ftp_pass) == strlen(cl->ftp_scratch_buffer));
				if (cl->ftp_data.loggin.passvalid) {
					ftp_send_reply(cl, 230, "Login successful.");
					break;
				}
			}
			ftp_send_reply(cl, 530, NULL);
			break;
		case E_FTP_CMD_PASV:
			{
				get_listen_addr(cl->ftp_data.c_sd, &cl->ftp_data.ip_addr);

				// some servers (e.g. google chrome) send PASV several times very quickly
				closesocket(cl->ftp_data.d_sd);
				cl->ftp_data.d_sd = -1;
				cl->ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
				bool socketcreated = true;
				if (cl->common->ld_sd < 0) {
					socketcreated = ftp_create_listening_socket(&cl->common->ld_sd, FTP_PASIVE_DATA_PORT, FTP_DATA_CLIENTS_MAX - 1);
				}
				if (socketcreated) {
					uint8_t *pip = (uint8_t *)&cl->ftp_data.ip_addr;
					cl->ftp_data.dtimeout = 0;
					snprintf((char *)cl->ftp_data.dBuffer, cl->ftp_buff_size, "Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
							 pip[0], pip[1], pip[2], pip[3], (FTP_PASIVE_DATA_PORT >> 8), (FTP_PASIVE_DATA_PORT & 0xFF));
					cl->ftp_data.substate = E_FTP_STE_SUB_LISTEN_FOR_DATA;
					ESP_LOGI(TAG_CL, "Data socket created");
					ftp_send_reply(cl, 227, (char *)cl->ftp_data.dBuffer);
				}
				else {
					ESP_LOGW(TAG_CL, "Error creating data socket");
					ftp_send_reply(cl, 425, NULL);
				}
			}
			break;
		case E_FTP_CMD_LIST:
		case E_FTP_CMD_NLST:
			ftp_get_param_and_open_child(cl, &bufptr);
			parse_list_params();
			if (cmd == E_FTP_CMD_LIST) cl->ftp_nlist = 0;
			else cl->ftp_nlist = 1;
			if (ftp_open_dir_for_listing(cl, cl->ftp_path) == E_FTP_RESULT_CONTINUE) {
				cl->ftp_data.state = E_FTP_STE_CONTINUE_LISTING;
				ftp_send_reply(cl, 150, "Here comes the directory listing.");
			}
			else ftp_send_reply(cl, 550, NULL);
			break;
		case E_FTP_CMD_RETR:
			cl->ftp_data.total = 0;
			cl->ftp_data.time = 0;
			ftp_get_param_and_open_child(cl, &bufptr);
			if ((strlen(cl->ftp_path) > 0) && (cl->ftp_path[strlen(cl->ftp_path)-1] != '/')) {
				if (ftp_open_file(cl, cl->ftp_path, "rb")) {
					cl->ftp_data.state = E_FTP_STE_CONTINUE_FILE_TX;
					vTaskDelay(20 / portTICK_PERIOD_MS);
					ftp_send_reply(cl, 150, NULL);
				}
				else {
					cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
					ftp_send_reply(cl, 550, NULL);
				}
			}
			else {
				cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
				ftp_send_reply(cl, 550, NULL);
			}
			break;
		case E_FTP_CMD_APPE:
			cl->ftp_data.total = 0;
			cl->ftp_data.time = 0;
			ftp_get_param_and_open_child(cl, &bufptr);
			if ((strlen(cl->ftp_path) > 0) && (cl->ftp_path[strlen(cl->ftp_path)-1] != '/')) {
				if (ftp_open_file(cl, cl->ftp_path, "ab")) {
					cl->ftp_data.state = E_FTP_STE_CONTINUE_FILE_RX;
					vTaskDelay(20 / portTICK_PERIOD_MS);
					ftp_send_reply(cl, 150, NULL);
				}
				else {
					cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
					ftp_send_reply(cl, 550, NULL);
				}
			}
			else {
				cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
				ftp_send_reply(cl, 550, NULL);
			}
			break;
		case E_FTP_CMD_STOR:
			cl->ftp_data.total = 0;
			cl->ftp_data.time = 0;
			ftp_get_param_and_open_child(cl, &bufptr);
			if ((strlen(cl->ftp_path) > 0) && (cl->ftp_path[strlen(cl->ftp_path)-1] != '/')) {
				ESP_LOGI(TAG_CL, "E_FTP_CMD_STOR cl->ftp_path=[%s]", cl->ftp_path);
				if (ftp_open_file(cl, cl->ftp_path, "wb")) {
					cl->ftp_data.state = E_FTP_STE_CONTINUE_FILE_RX;
					vTaskDelay(20 / portTICK_PERIOD_MS);
					ftp_send_reply(cl, 150, NULL);
				}
				else {
					cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
					ftp_send_reply(cl, 550, NULL);
				}
			}
			else {
				cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
				ftp_send_reply(cl, 550, NULL);
			}
			break;
		case E_FTP_CMD_DELE:
			ftp_get_param_and_open_child(cl, &bufptr);
			if ((strlen(cl->ftp_path) > 0) && (cl->ftp_path[strlen(cl->ftp_path)-1] != '/')) {
				ESP_LOGI(TAG_CL, "E_FTP_CMD_DELE cl->ftp_path=[%s]", cl->ftp_path);

				strcat(fullname, cl->ftp_path);
				ESP_LOGI(TAG_CL, "E_FTP_CMD_DELE fullname=[%s]", fullname);

				//if (unlink(cl->ftp_path) == 0) {
				if (unlink(fullname) == 0) {
					vTaskDelay(20 / portTICK_PERIOD_MS);
					ftp_send_reply(cl, 250, MSG_250);
				}
				else ftp_send_reply(cl, 550, NULL);
			}
			else ftp_send_reply(cl, 250, MSG_250);
			break;
		case E_FTP_CMD_RMD:
			ftp_get_param_and_open_child(cl, &bufptr);
			if ((strlen(cl->ftp_path) > 0) && (cl->ftp_path[strlen(cl->ftp_path)-1] != '/')) {
				ESP_LOGI(TAG_CL, "E_FTP_CMD_RMD cl->ftp_path=[%s]", cl->ftp_path);

				strcat(fullname, cl->ftp_path);
				ESP_LOGI(TAG_CL, "E_FTP_CMD_MKD fullname=[%s]", fullname);

				//if (rmdir(cl->ftp_path) == 0) {
				if (rmdir(fullname) == 0) {
					vTaskDelay(20 / portTICK_PERIOD_MS);
					ftp_send_reply(cl, 250, MSG_250);
				}
				else ftp_send_reply(cl, 550, NULL);
			}
			else ftp_send_reply(cl, 250, MSG_250);
			break;
		case E_FTP_CMD_MKD:
			ftp_get_param_and_open_child(cl, &bufptr);
			if ((strlen(cl->ftp_path) > 0) && (cl->ftp_path[strlen(cl->ftp_path)-1] != '/')) {
				ESP_LOGI(TAG_CL, "E_FTP_CMD_MKD cl->ftp_path=[%s]", cl->ftp_path);

				strcat(fullname, cl->ftp_path);
				ESP_LOGI(TAG_CL, "E_FTP_CMD_MKD fullname=[%s]", fullname);

				//if (mkdir(cl->ftp_path, 0755) == 0) {
				if (mkdir(fullname, 0755) == 0) {
					vTaskDelay(20 / portTICK_PERIOD_MS);
					ftp_send_reply(cl, 250, MSG_250);
				}
				else ftp_send_reply(cl, 550, NULL);
			}
			else ftp_send_reply(cl, 250, MSG_250);
			break;
		case E_FTP_CMD_RNFR:
			ftp_get_param_and_open_child(cl, &bufptr);
			ESP_LOGI(TAG_CL, "E_FTP_CMD_RNFR cl->ftp_path=[%s]", cl->ftp_path);

			strcat(fullname, cl->ftp_path);
			ESP_LOGI(TAG_CL, "E_FTP_CMD_MKD fullname=[%s]", fullname);

			//res = stat(cl->ftp_path, &buf);
			res = stat(fullname, &buf);
			if (res == 0) {
				ftp_send_reply(cl, 350, NULL);
				// save the path of the file to rename
				strcpy((char *)cl->ftp_data.dBuffer, cl->ftp_path);
			} else {
				ftp_send_reply(cl, 550, NULL);
			}
			break;
		case E_FTP_CMD_RNTO:
			ftp_get_param_and_open_child(cl, &bufptr);
			// the path of the file to rename was saved in the data buffer
			ESP_LOGI(TAG_CL, "E_FTP_CMD_RNTO cl->ftp_path=[%s], cl->ftp_data.dBuffer=[%s]", cl->ftp_path, (char *)cl->ftp_data.dBuffer);
			strcat(fullname, (char *)cl->ftp_data.dBuffer);
			ESP_LOGI(TAG_CL, "E_FTP_CMD_RNTO fullname=[%s]", fullname);
			strcat(fullname2, cl->ftp_path);
			ESP_LOGI(TAG_CL, "E_FTP_CMD_RNTO fullname2=[%s]", fullname2);

			//if (rename((char *)cl->ftp_data.dBuffer, cl->ftp_path) == 0) {
			if (rename(fullname, fullname2) == 0) {
				ftp_send_reply(cl, 250, MSG_250);
			} else {
				ftp_send_reply(cl, 550, NULL);
			}
			break;
		case E_FTP_CMD_NOOP:
			ftp_send_reply(cl, 200, NULL);
			break;
		case E_FTP_CMD_QUIT:
			ftp_send_reply(cl, 221, NULL);
			break;
		default:
			// command not implemented
			ftp_send_reply(cl, 502, NULL);
			break;
		}

		if (cl->ftp_data.closechild) {
			remove_fname_from_path(cl->ftp_path, cl->ftp_scratch_buffer);
		}
	}
	else if (result == E_FTP_RESULT_CONTINUE) {
		if (cl->ftp_data.ctimeout > FTP_CMD_TIMEOUT_MS) {
			ftp_send_reply(cl, 221, NULL);
			ESP_LOGW(TAG_CL, "Connection timeout");
		}
	}
	else {
		ftp_close_cmd_data(cl);
	}
}

//---------------------------------------
static void ftp_wait_for_enabled (PCLIENT cl) {
	// Check if the telnet service has been enabled
	if (cl->ftp_data.enabled) {
		cl->ftp_data.state = E_FTP_STE_START;
	}
}

// ==== PUBLIC FUNCTIONS ===================================================================

//---------------------
void ftp_deinit(PCLIENT cl) {
	if (cl->ftp_path) free(cl->ftp_path);
	if (cl->ftp_cmd_buffer) free(cl->ftp_cmd_buffer);
	if (cl->ftp_data.dBuffer) free(cl->ftp_data.dBuffer);
	if (cl->ftp_scratch_buffer) free(cl->ftp_scratch_buffer);
	cl->ftp_path = NULL;
	cl->ftp_cmd_buffer = NULL;
	cl->ftp_data.dBuffer = NULL;
	cl->ftp_scratch_buffer = NULL;
}

//-------------------
bool ftp_init(PCLIENT cl) {
	ftp_stop = 0;
	// Allocate memory for the data buffer, and the file system structures (from the RTOS heap)
	ftp_deinit(cl);

	memset(&cl->ftp_data, 0, sizeof(ftp_data_t));

	cl->ftp_buff_size = CONFIG_MICROPY_FTPSERVER_BUFFER_SIZE;
	cl->ftp_data.dBuffer = malloc(cl->ftp_buff_size+1);
	if (cl->ftp_data.dBuffer == NULL) return false;
	cl->ftp_path = malloc(FTP_MAX_PARAM_SIZE);
	if (cl->ftp_path == NULL) {
		free(cl->ftp_data.dBuffer);
		return false;
	}
	cl->ftp_scratch_buffer = malloc(FTP_MAX_PARAM_SIZE);
	if (cl->ftp_scratch_buffer == NULL) {
		free(cl->ftp_path);
		free(cl->ftp_data.dBuffer);
		return false;
	}
	cl->ftp_cmd_buffer = malloc(FTP_MAX_PARAM_SIZE + FTP_CMD_SIZE_MAX);
	if (cl->ftp_cmd_buffer == NULL) {
		free(cl->ftp_scratch_buffer);
		free(cl->ftp_path);
		free(cl->ftp_data.dBuffer);
		return false;
	}

	//SOCKETFIFO_Init((void *)ftp_fifoelements, FTP_SOCKETFIFO_ELEMENTS_MAX);

	cl->ftp_data.c_sd  = -1;
	cl->ftp_data.d_sd  = -1;
	cl->ftp_data.e_open = E_FTP_NOTHING_OPEN;
	cl->ftp_data.state = E_FTP_STE_DISABLED;
	cl->ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;

	return true;
}

//============================
int ftp_run (PCLIENT  cl, uint32_t elapsed)
{
	//if (xSemaphoreTake(ftp_mutex, FTP_MUTEX_TIMEOUT_MS / portTICK_PERIOD_MS) !=pdTRUE) return -1;
	if (ftp_stop) return -2;

	cl->ftp_data.dtimeout += elapsed;
	cl->ftp_data.ctimeout += elapsed;
	cl->ftp_data.time += elapsed;

	//ESP_LOGI(TAG_CL, "FTP%.2d state = %d ", cl->num, cl->ftp_data.state);

	switch (cl->ftp_data.state) {
		case E_FTP_STE_DISABLED:
			ftp_wait_for_enabled(cl);
			break;
		case E_FTP_STE_START:
//			if (ftp_create_listening_socket(&cl->ftp_data.lc_sd, FTP_CMD_PORT, FTP_CMD_CLIENTS_MAX - 1)) {
				cl->ftp_data.state = E_FTP_STE_READY;
//			}
			break;
		case E_FTP_STE_READY:
			if (cl->ftp_data.c_sd < 0 && cl->ftp_data.substate == E_FTP_STE_SUB_DISCONNECTED) {
				//ESP_LOGI(TAG_CL, "FTP%.2d wait for connection ...", cl->num);
				if (E_FTP_RESULT_OK == ftp_wait_for_connection(cl, cl->common->lc_sd, &cl->ftp_data.c_sd, &cl->ftp_data.ip_addr)) {
					cl->ftp_data.txRetries = 0;
					cl->ftp_data.logginRetries = 0;
					cl->ftp_data.ctimeout = 0;
					cl->ftp_data.loggin.useranon  = false;
					cl->ftp_data.loggin.uservalid = false;
					cl->ftp_data.loggin.passvalid = false;
					strcpy (cl->ftp_path, "/");
					ESP_LOGI(TAG_CL, "Connected.");
					cl->inUse = true;
					ftp_send_reply(cl, 220, "ModuleBox FTP Server");
					break;
				}
			}
			if (cl->ftp_data.c_sd > 0 && cl->ftp_data.substate != E_FTP_STE_SUB_LISTEN_FOR_DATA) {
				ftp_process_cmd(cl);
				if (cl->ftp_data.state != E_FTP_STE_READY) {
					break;
				}
			}
			break;
		case E_FTP_STE_END_TRANSFER:
			if (cl->ftp_data.d_sd >= 0) {
				closesocket(cl->ftp_data.d_sd);
				cl->ftp_data.d_sd = -1;
			}
			break;
		case E_FTP_STE_CONTINUE_LISTING:
			// go on with listing
			{
				uint32_t listsize = 0;
				ftp_result_t list_res = ftp_list_dir(cl, &listsize);
				if (listsize > 0) ftp_send_list(cl, listsize);
				if (list_res == E_FTP_RESULT_OK) {
					ftp_send_reply(cl, 226, "Directory send OK.");
					cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
				}
				cl->ftp_data.ctimeout = 0;
			}
			break;
		case E_FTP_STE_CONTINUE_FILE_TX:
			// read and send the next block from the file
			{
				uint32_t readsize;
				ftp_result_t result;
				cl->ftp_data.ctimeout = 0;
				result = ftp_read_file (cl, (char *)cl->ftp_data.dBuffer, cl->ftp_buff_size, &readsize);
				if (result == E_FTP_RESULT_FAILED) {
					ftp_send_reply(cl, 451, NULL);
					cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
				}
				else {
					if (readsize > 0) {
						ftp_send_file_data(cl, readsize);
						cl->ftp_data.total += readsize;
						ESP_LOGI(TAG_CL, "Sent %"PRIu32", total: %"PRIu32, readsize, cl->ftp_data.total);
					}
					if (result == E_FTP_RESULT_OK) {
						ftp_send_reply(cl, 226, NULL);
						cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
						ESP_LOGI(TAG_CL, "File sent (%"PRIu32" bytes in %"PRIu32" msec).", cl->ftp_data.total, cl->ftp_data.time);
					}
				}
			}
			break;
		case E_FTP_STE_CONTINUE_FILE_RX:
			{
				int32_t len;
				ftp_result_t result = E_FTP_RESULT_OK;

				//ESP_LOGI(TAG_CL, "cl->ftp_buff_size=%d", cl->ftp_buff_size);
				result = ftp_recv_non_blocking(cl->ftp_data.d_sd, cl->ftp_data.dBuffer, cl->ftp_buff_size, &len);
				if (result == E_FTP_RESULT_OK) {
					// block of data received
					cl->ftp_data.dtimeout = 0;
					cl->ftp_data.ctimeout = 0;
					// save received data to file
					if (E_FTP_RESULT_OK != ftp_write_file (cl, (char *)cl->ftp_data.dBuffer, len)) {
						ftp_send_reply(cl, 451, NULL);
						cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
						ESP_LOGW(TAG_CL, "Error writing to file");
					}
					else {
						cl->ftp_data.total += len;
						ESP_LOGI(TAG_CL, "Received %"PRIu32", total: %"PRIu32, len, cl->ftp_data.total);
					}
				}
				else if (result == E_FTP_RESULT_CONTINUE) {
					// nothing received
					if (cl->ftp_data.dtimeout > FTP_DATA_TIMEOUT_MS) {
						ftp_close_files_dir(cl);
						ftp_send_reply(cl, 426, NULL);
						cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
						ESP_LOGW(TAG_CL, "Receiving to file timeout");
					}
				}
				else {
					// File received (E_FTP_RESULT_FAILED)
					ftp_close_files_dir(cl);
					ftp_send_reply(cl, 226, NULL);
					cl->ftp_data.state = E_FTP_STE_END_TRANSFER;
					ESP_LOGI(TAG_CL, "File received (%"PRIu32" bytes in %"PRIu32" msec).", cl->ftp_data.total, cl->ftp_data.time);
					break;
				}
			}
			break;
		default:
			break;
	}

	switch (cl->ftp_data.substate) {
	case E_FTP_STE_SUB_DISCONNECTED:
		break;
	case E_FTP_STE_SUB_LISTEN_FOR_DATA:
		if (E_FTP_RESULT_OK == ftp_wait_for_connection(cl, cl->common->ld_sd, &cl->ftp_data.d_sd, NULL)) {
			cl->ftp_data.dtimeout = 0;
			cl->ftp_data.substate = E_FTP_STE_SUB_DATA_CONNECTED;
			ESP_LOGI(TAG_CL, "Data socket connected");
		}
		else if (cl->ftp_data.dtimeout > FTP_DATA_TIMEOUT_MS) {
			ESP_LOGW(TAG_CL, "Waiting for data connection timeout (%"PRIi32")", cl->ftp_data.dtimeout);
			cl->ftp_data.dtimeout = 0;
			// close the listening socket
			//closesocket(cl->ftp_data.ld_sd);
			//cl->ftp_data.ld_sd = -1;
			cl->ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
		}
		break;
	case E_FTP_STE_SUB_DATA_CONNECTED:
		if (cl->ftp_data.state == E_FTP_STE_READY && (cl->ftp_data.dtimeout > FTP_DATA_TIMEOUT_MS)) {
			// close the listening and the data socket
			// closesocket(cl->ftp_data.ld_sd);
			// closesocket(cl->ftp_data.d_sd);
			// cl->ftp_data.ld_sd = -1;
			// cl->ftp_data.d_sd = -1;
			ftp_close_filesystem_on_error(cl);
			cl->ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
			ESP_LOGW(TAG_CL, "Data connection timeout");
		}
		break;
	default:
		break;
	}

	// check the state of the data sockets
	if (cl->ftp_data.d_sd < 0 && (cl->ftp_data.state > E_FTP_STE_READY)) {
		cl->ftp_data.substate = E_FTP_STE_SUB_DISCONNECTED;
		cl->ftp_data.state = E_FTP_STE_READY;
		ESP_LOGI(TAG_CL, "Data socket disconnected");
	}

	//xSemaphoreGive(ftp_mutex);
	return 0;
}

//----------------------
bool ftp_enable (PCLIENT cl) {
	bool res = false;
	if (cl->ftp_data.state == E_FTP_STE_DISABLED) {
		cl->ftp_data.enabled = true;
		res = true;
	}
	return res;
}

//-------------------------
bool ftp_isenabled (PCLIENT cl) {
	bool res = (cl->ftp_data.enabled == true);
	return res;
}

//-----------------------
bool ftp_disable (PCLIENT cl) {
	bool res = false;
	if (cl->ftp_data.state == E_FTP_STE_READY) {
		_ftp_reset(cl);
		cl->ftp_data.enabled = false;
		cl->ftp_data.state = E_FTP_STE_DISABLED;
		res = true;
	}
	return res;
}

//---------------------
bool ftp_reset (PCLIENT cl) {
	_ftp_reset(cl);
	return true;
}

// Return current ftp server state
//------------------
int ftp_getstate(PCLIENT cl) {
	int fstate = cl->ftp_data.state | (cl->ftp_data.substate << 8);
	if ((cl->ftp_data.state == E_FTP_STE_READY) && (cl->ftp_data.c_sd > 0)) fstate = E_FTP_STE_CONNECTED;
	return fstate;
}

//-------------------------
bool ftp_terminate (PCLIENT cl) {
	bool res = false;
	if (cl->ftp_data.state == E_FTP_STE_READY) {
		ftp_stop = 1;
		_ftp_reset(cl);
		res = true;
	}
	return res;
}

//-------------------------
bool ftp_stop_requested() {
	bool res = (ftp_stop == 1);
	return res;
}

#if 0
//-------------------------------
int32_t ftp_get_maxstack (void) {
	int32_t maxstack = ftp_stack_size - uxTaskGetStackHighWaterMark(NULL);
	return maxstack;
}
#endif


//-------------------------------
void ftp_task(void *pvParameters) {

	uint32_t startTick = xTaskGetTickCount();
	uint32_t heapBefore = xPortGetFreeHeapSize();
	char *  resetCause	= 0;
	int 	resetTimeout = 0;

//	shared_var_mutex = xSemaphoreCreateMutex();  // Create the mutex

	anon_enabled = me_config.FTP_anon;
	strcpy(ftp_user, me_config.FTP_login);
	strcpy(ftp_pass, me_config.FTP_pass);
	ESP_LOGI(TAG_CL, "ftp_user:[%s] ftp_pass:[%s], anon enabled:[%s]", ftp_user, ftp_pass, anon_enabled ? "yes" : "no");

	net_if_num = network_get_active_interfaces();
	ESP_LOGI(TAG_CL, "network_get_active_interfaces n_if=%d", net_if_num);


	if (!ftp_create_listening_socket(&ftp_common.lc_sd, FTP_CMD_PORT, FTP_CMD_CLIENTS_MAX - 1)) 
	{
		ESP_LOGE(TAG_CL, "Socket init Error");
		vTaskDelete(NULL);
	}

	for (int i = 0; i < MAX_CLIENTS; i++)
	{
		memset(&cl[i], 0, sizeof(CLIENT));

		cl[i].num		= i + 1;
		cl[i].common 	= &ftp_common;

		if (!ftp_init(&cl[i])) 
		{
			ESP_LOGE(TAG_CL, "Init Error");
			//xEventGroupSetBits(xEventTask, FTP_TASK_FINISH_BIT);
			vTaskDelete(NULL);
		}

		ftp_enable(&cl[i]);
	}

	uint64_t now, time_ms = mp_hal_ticks_ms();
	// Initialize ftp, create rx buffer and mutex

	// We have network connection, enable ftp

	time_ms = mp_hal_ticks_ms();

	ESP_LOGD(TAG_CL, "FTP init complite. Duration: %ld ms. Heap usage: %lu", (xTaskGetTickCount() - startTick) * portTICK_RATE_MS, heapBefore - xPortGetFreeHeapSize());
	me_state.FTP_init_res = ESP_OK;
	while (1) 
	{
// -----------------------------------------------------------------		
		// Calculate time between two ftp_run() calls
		now = mp_hal_ticks_ms();


		// ESP_LOGE(TAG_CL, "state: %d %d %d %d %d", 
		// 		ftp_getstate(&cl[0]),
		// 		ftp_getstate(&cl[1]),
		// 		ftp_getstate(&cl[2]),
		// 		ftp_getstate(&cl[3]),
		// 		ftp_getstate(&cl[4])
		// );


// -----------------------------------------------------------------

		int anyInUse 		= 0;
		int anyReqToReset	= 0;

		for (int i = 0; i < MAX_CLIENTS; i++)
		{
			int res = ftp_run(&cl[i], now - time_ms);
			if (res < 0) {
				if (res == -1) {
					ESP_LOGE(TAG_CL, "\nRun Error");
				}
				// -2 is returned if Ftp stop was requested by user
				resetCause = "runtume error";
				resetTimeout = 0;
			}

			if (cl[i].resetTrigger)
				anyReqToReset++;

			if (cl[i].inUse)
				anyInUse++;
		}

		time_ms = now;

		if (!resetCause)
		{
			if (anyReqToReset && !anyInUse)
			{
				resetCause = "all connections are terminated";

				if (!resetTimeout)
					resetTimeout = DEF_RESET_TIMEOUT;
			}
			// else
			// 	ESP_LOGI(TAG_CL, "CHECK: anyInUse: %d, anyReqToReset: %d", anyInUse, anyReqToReset);
		}

		if (resetCause)
		{
			if (!resetTimeout || !(--resetTimeout))
			{
				ESP_LOGI(TAG_CL, "RESET by FTP: %s", resetCause);
				esp_restart();
			}
			// else
			// 	ESP_LOGI(TAG_CL, "RESET timeout: %d", resetTimeout);			
		}
		else
		{
			if (anyInUse)
				vTaskDelay(1);
			else
				vTaskDelay(pdMS_TO_TICKS(20));
		}

	} // end while

	ESP_LOGW(TAG_CL, "\nTask terminated!");
	//xEventGroupSetBits(xEventTask, FTP_TASK_FINISH_BIT);
	vTaskDelete(NULL);
}
