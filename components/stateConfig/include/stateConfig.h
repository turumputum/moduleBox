#include "leds.h"
//#include "freertos/task.h"
#include <ff.h>

#define MAX_NUM_OF_TRACKS 10
#define FILE_NAME_LEGHT 300
#define INIT_OK 0
#define INIT_FAIL -1

#define NUM_OF_SLOTS 6

static const char* VERSION = "1.0";

typedef enum {
    LED_STATE_DISABLE = 0,
    LED_STATE_SD_ERROR,
    LED_STATE_CONFIG_ERROR,
    LED_STATE_CONTENT_ERROR,
	LED_STATE_SENSOR_ERROR,
    LED_STATE_WIFI_FAIL,
	LED_STATE_WIFI_OK,
    LED_STATE_STANDBY,
    LED_STATE_PLAY,
    LED_STATE_FTP_SESSION,
	LED_STATE_MSD_WORK,
} led_state_t;

typedef struct {
//	uint8_t changeTrack;
	uint8_t currentTrack;
	uint8_t numOfTrack;
//	uint8_t phoneUp;
//	uint8_t prevPhoneUp;

	char * wifiApClientString;

	int8_t sd_init_res;
	int8_t config_init_res;
	int8_t content_search_res;
	int8_t slot_init_res;
	int8_t WIFI_init_res;
	int8_t LAN_init_res;
	int8_t MQTT_init_res;

	int8_t udp_socket;
	int8_t osc_socket;

//	led_state_t bt_state_mass[8];
//	led_state_t ledState;

	TaskHandle_t slot_task[8];

	char *triggers_topic_list[16];
	uint8_t triggers_topic_list_index;
	char *action_topic_list[16];
	uint8_t action_topic_list_index;

} stateStruct;



typedef struct {
	TCHAR audioFile[FILE_NAME_LEGHT];
	TCHAR icoFile[FILE_NAME_LEGHT];
} track_t;

typedef TCHAR file_t[FILE_NAME_LEGHT];

typedef struct {
	uint8_t WIFI_mode; 
	char * WIFI_ssid;
	char * WIFI_pass;
	uint8_t WIFI_channel;
	
	uint8_t LAN_enable;

	uint8_t DHCP;

	char *ipAdress;
	char *netMask;
	char *gateWay;

	char *device_name;

	uint8_t FTP_enable;
	char *FTP_login;
	char *FTP_pass;

	char *udpServerAdress;
	uint16_t udpServerPort;
	uint16_t udpMyPort;

	char *oscServerAdress;
	uint16_t oscServerPort;
	uint16_t oscMyPort;

	uint8_t MDNS_enable;

	char *mqttBrokerAdress;
	char *mqttLogin;
	char *mqttPass;

	uint8_t monofonEnable;

	uint16_t play_delay;
	uint8_t loop;
	uint8_t volume;

	file_t soundTracks[MAX_NUM_OF_TRACKS];
	file_t trackIcons[MAX_NUM_OF_TRACKS];

	char *slot_mode[NUM_OF_SLOTS];
	char *slot_options[NUM_OF_SLOTS];
	char *slot_cross_link[NUM_OF_SLOTS];

	char *startup_cross_link;

	char configFile[FILE_NAME_LEGHT];
	char introIco[FILE_NAME_LEGHT];

	uint8_t f_report_udp;
	uint8_t f_report_osc;

	char ssidT[33];

} configuration;


uint8_t loadConfig(void);
FRESULT scan_in_dir(const char *file_extension, FF_DIR *dp, FILINFO *fno);
void load_Default_Config(void);
void writeErrorTxt(const char *buff);
uint8_t loadContent(void);
int saveConfig(void);

uint8_t scanFileSystem();
uint8_t scan_dir(const char *path);
