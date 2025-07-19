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


#include "stateConfig.h"

#include "LAN.h"
#include "ftp.h"
#include "myMqtt.h"
#include "tinyosc.h"
#include "reporter.h"
#include "executor.h"

#include "mdns.h"

#define CONFIG_ETH_SPI_ETHERNET_W5500 1

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "LAN";


void ftp_task(void *pvParameters);


typedef struct {
    uint8_t spi_cs_gpio;
    uint8_t int_gpio;
    int8_t phy_reset_gpio;
    uint8_t phy_addr;
    uint8_t *mac_addr;
}spi_eth_module_config_t;

extern configuration me_config;
extern stateStruct me_state;
//extern QueueHandle_t exec_mailbox;

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	uint8_t mac_addr[6] = { 0 };
	/* we can get the ethernet driver handle from event data */
	esp_eth_handle_t eth_handle = *(esp_eth_handle_t*) event_data;

	switch (event_id) {
	case ETHERNET_EVENT_CONNECTED:
		esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
		ESP_LOGI(TAG, "Ethernet Link Up");
		ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x", mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
		break;
	case ETHERNET_EVENT_DISCONNECTED:
		ESP_LOGI(TAG, "Ethernet Link Down");
		break;
	case ETHERNET_EVENT_START:
		ESP_LOGI(TAG, "Ethernet Started");
		break;
	case ETHERNET_EVENT_STOP:
		ESP_LOGI(TAG, "Ethernet Stopped");
		break;
	default:
		break;
	}
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
	const esp_netif_ip_info_t *ip_info = &event->ip_info;
	me_state.LAN_init_res = ESP_OK;

	ESP_LOGI(TAG, "Ethernet Got IP Address");
	ESP_LOGI(TAG, "~~~~~~~~~~~");
	ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
	ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
	ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
	ESP_LOGI(TAG, "~~~~~~~~~~~");
}

//-----------------------OSC-----------------------------------
void osc_recive_task(){
	if((strlen(me_config.oscServerAdress) < 7)||(me_config.oscMyPort < 1)){
		ESP_LOGD(TAG, "wrong OSC config");
		vTaskDelay(pdMS_TO_TICKS(200));
		vTaskDelete(NULL);
	}

	me_state.osc_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (me_state.osc_socket < 0) {
		printf("Failed to create socket for OSC: %d\n", errno);
		writeErrorTxt("Failed to create socket for OSC");
		vTaskDelete(NULL);
	}else{
		ESP_LOGD(TAG,"OSC socket OK num:%d", me_state.osc_socket);
	}

	// struct timeval timeout;
	// timeout.tv_sec = 10;
	// timeout.tv_usec = 0;
	// setsockopt (me_state.osc_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

	int buff_size=250;
	//char buff[buff_size];
	//char *buffForRecive=(char*)malloc(buff_size);
	char *buffForRecive=heap_caps_calloc(1,buff_size,MALLOC_CAP_SPIRAM);
	//char *sringForReport=(char*)malloc(buff_size);
	char *sringForReport=heap_caps_calloc(1,buff_size,MALLOC_CAP_SPIRAM);
	//calloc(buff,buff_size);

	tosc_message osc;

	struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
    socklen_t socklen = sizeof(source_addr);

	struct sockaddr_in dest_addr;
	dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(me_config.oscMyPort);

	int err = bind(me_state.osc_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
	if (err < 0) {
		ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
	}

	ESP_LOGD(TAG, "OSC revive task STARTED, on port:%d",me_config.oscMyPort);
	me_state.OSC_init_res = ESP_OK;
	while(1){
		int len=recvfrom(me_state.osc_socket, buffForRecive, buff_size-1, 0,(struct sockaddr *)&source_addr, &socklen);
		if(len<0){
			//ESP_LOGD(TAG, "OSC incoming fail(");
		}else{
			ESP_LOGD(TAG, "OSC incoming:%s", buffForRecive);
			if (!tosc_parseMessage(&osc, buffForRecive, len)) {
				//printf("Received OSC message: [%i bytes] %s %s ",
					//len, // the number of bytes in the OSC message
					//tosc_getAddress(&osc), // the OSC address string, e.g. "/button1"
					
					tosc_getFormat(&osc); // the OSC format string, e.g. "f"
				for (int i = 0; osc.format[i] != '\0'; i++) {
					if(osc.format[i]== 'i'){
						
						//char strT[255];
						sprintf(sringForReport, "%s:%ld", tosc_getAddress(&osc), tosc_getNextInt32(&osc));
						char *temp = sringForReport;
						if (strstr(temp, me_config.deviceName) != NULL) {
							temp += 1;
						}
						execute(temp);
						
					} 
					if(osc.format[i]== 'f'){
						//printf("%i ", ); break;
						//char strT[255];
						sprintf(sringForReport, "%s:%f", tosc_getAddress(&osc), tosc_getNextFloat(&osc));
						char *temp = sringForReport;
						if (strstr(temp, me_config.deviceName) != NULL) {
							temp += 1;
						}
						execute(temp);
					} 
				}
			}
		}
		// UBaseType_t stack_remaining = uxTaskGetStackHighWaterMark(NULL);
        // ESP_LOGI(TAG, "Stack remaining: %u", stack_remaining);
		vTaskDelay(pdMS_TO_TICKS(25));
	}
}

void start_osc_recive_task(){
	xTaskCreatePinnedToCore(osc_recive_task, "osc_recive_task", 1024 * 6, NULL, configMAX_PRIORITIES - 10, NULL,0);
}




//------------------------------FTP----------------------------------------------
void start_ftp_task(){

	if(me_config.FTP_enable){

		uint32_t startTick = xTaskGetTickCount();
		uint32_t heapBefore = xPortGetFreeHeapSize();
		xTaskCreatePinnedToCore(ftp_task, "FTP", 1024 * 6, NULL, 4, NULL, 0);
		ESP_LOGD(TAG, "FTP task started. Duration: %ld ms. Heap usage: %lu free heap:%u", (xTaskGetTickCount() - startTick) * portTICK_PERIOD_MS, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
	}
}


//------------------------------MDNS----------------------------------------------
void start_mdns_task(){

	if(me_config.MDNS_enable){
		uint32_t startTick = xTaskGetTickCount();
		uint32_t heapBefore = xPortGetFreeHeapSize();

	 	ESP_ERROR_CHECK( mdns_init() );


		ESP_ERROR_CHECK( mdns_hostname_set(me_config.deviceName));
		ESP_ERROR_CHECK( mdns_instance_name_set(me_config.deviceName));

		//ESP_ERROR_CHECK( mdns_query_a(me_config.deviceName, 2000,  &addr));


		//mdns_query_ptr()
		//initialize service
		char tmp[strlen(me_config.deviceName)+strlen("FTP server on")+5];
		sprintf(tmp,"FTP server on %s",me_config.deviceName);
		ESP_ERROR_CHECK( mdns_service_add(tmp, "_ftp", "_tcp", 21, 0, 0) );
		ESP_LOGD(TAG, "mDNS task started. Duration: %ld ms. Heap usage: %lu free heap:%u", (xTaskGetTickCount() - startTick) * portTICK_PERIOD_MS, heapBefore - xPortGetFreeHeapSize(), xPortGetFreeHeapSize());
		
	}
}


//------------------------------MQTT----------------------------------------------
void start_mqtt_task(){
	if (strlen(me_config.mqttBrokerAdress) > 3)	{
		mqtt_app_start();
	}else{
		ESP_LOGD(TAG, "MQTT wrong config");
	}
}


int LAN_init(void) {
	me_state.LAN_init_res = -1;

	esp_err_t ret = ESP_OK;

	// Install GPIO ISR handler to be able to service SPI Eth modules interrupts
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "GPIO ISR handler has been already installed");
            ret = ESP_OK; // ISR handler has been already installed so no issues
        } else {
            ESP_LOGE(TAG, "GPIO ISR handler install failed");
            return ret;
        }
    }

    // Init SPI bus
    spi_bus_config_t buscfg = {
        .miso_io_num = 13,
        .mosi_io_num = 9,
        .sclk_io_num = 12,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ret = spi_bus_initialize(1, &buscfg, SPI_DMA_CH_AUTO);
	if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPI host #%d init failed", ret);
		return ret;
	}

	spi_eth_module_config_t spi_eth_module_config;
	spi_eth_module_config.spi_cs_gpio = 11;           \
    spi_eth_module_config.int_gpio = 16;             \
    spi_eth_module_config.phy_reset_gpio = 14;   \
    spi_eth_module_config.phy_addr = 1;                \
	spi_eth_module_config.mac_addr = NULL;

	uint8_t base_mac_addr[ETH_ADDR_LEN];

	ret = esp_efuse_mac_get_default(base_mac_addr);
	if (ret != ESP_OK) {
        ESP_LOGW(TAG, "get EFUSE MAC failed, ret:%d", ret);
		return ret;
	}

	uint8_t local_mac_1[ETH_ADDR_LEN];
	esp_derive_local_mac(local_mac_1, base_mac_addr);
	spi_eth_module_config.mac_addr = local_mac_1;

	 // Init common MAC and PHY configs to default
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

	// Update PHY config based on board specific configuration
    phy_config.phy_addr = spi_eth_module_config.phy_addr;
    phy_config.reset_gpio_num = spi_eth_module_config.phy_reset_gpio;

    // Configure SPI interface for specific SPI module
    spi_device_interface_config_t spi_devcfg = {
        .mode = 0,
        .clock_speed_hz = 36 * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = spi_eth_module_config.spi_cs_gpio
    };

	eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(1, &spi_devcfg);
    w5500_config.int_gpio_num = spi_eth_module_config.int_gpio;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

	vTaskDelay(pdMS_TO_TICKS(100));

	esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t eth_config_spi = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_config_spi, &eth_handle);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "SPI Ethernet driver install failed, ret:%d", ret);
		//return ret;
	}

	if (spi_eth_module_config.mac_addr != NULL) {
        ret = esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, spi_eth_module_config.mac_addr);
		if (ret != ESP_OK) {
			ESP_LOGW(TAG, "SPI Ethernet MAC address config failed, ret:%d", ret);
			return ret;
		}
    }

	// Initialize TCP/IP network interface aka the esp-netif (should be called only once in application)
    ESP_ERROR_CHECK(esp_netif_init());
    // Create default event loop that running in background
    ESP_ERROR_CHECK(esp_event_loop_create_default());

	esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    // Attach Ethernet driver to TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

	// Register user defined event handers
	ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

	ESP_ERROR_CHECK(esp_eth_start(eth_handle));

	esp_netif_ip_info_t info_t;
	if (me_config.LAN_DHCP == 0) {
		ip4addr_aton((const char*) me_config.LAN_ipAdress, &info_t.ip);
		ip4addr_aton((const char*) me_config.LAN_gateWay, &info_t.gw);
		ip4addr_aton((const char*) me_config.LAN_netMask, &info_t.netmask);
		esp_netif_dhcpc_stop(eth_netif);
		esp_netif_set_ip_info(eth_netif, &info_t);
	}


	





	//xTaskCreatePinnedToCore(wait_lan, "wait_lan", 1024 * 4, NULL, configMAX_PRIORITIES - 8, NULL, 0);
	//xTaskCreate(wait_lan, "wait_lan", 1024 * 4, NULL, configMAX_PRIORITIES - 8, NULL);
	vTaskDelay(pdMS_TO_TICKS(1000));

	return 0;
}
