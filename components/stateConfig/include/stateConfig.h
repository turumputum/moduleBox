//#include "leds.h"
//#include "freertos/task.h"
#include <ff.h>

#include <stdint.h>
#include <schedule_parser.h>

#define MAX_NUM_OF_TRACKS 10
#define FILE_NAME_LEGHT 300
#define INIT_OK 0
#define INIT_FAIL -1

#define NUM_OF_SLOTS 10

#define VERSION 	"3.311" BRANCH

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
//#define LEDC_OUTPUT_IO          (10) // Define the output GPIO
//#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_8_BIT // Set duty resolution to 13 bits
#define LEDC_FREQUENCY          (5000) // Frequency in Hertz. Set frequency at 5 kHz


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
	int8_t UDP_init_res;
	int8_t OSC_init_res;
	int8_t FTP_init_res;

	int8_t eth_connected;


	int8_t udplink_socket;
	int8_t osc_socket;

	int8_t free_i2c_num;
	int8_t ledc_chennelCounter;

//	led_state_t bt_state_mass[8];
//	led_state_t ledState;
	QueueHandle_t executor_queue;
	QueueHandle_t reporter_queue;
	QueueHandle_t reporter_spread_queue;

	QueueHandle_t command_queue[NUM_OF_SLOTS];
	QueueHandle_t interrupt_queue[NUM_OF_SLOTS];  

	TaskHandle_t slot_task[NUM_OF_SLOTS];

	char *trigger_topic_list[NUM_OF_SLOTS];
	char *action_topic_list[NUM_OF_SLOTS];

} stateStruct;


typedef struct {
	TCHAR audioFile[FILE_NAME_LEGHT];
	TCHAR icoFile[FILE_NAME_LEGHT];
} track_t;

typedef TCHAR file_t[FILE_NAME_LEGHT];

// Schedule entry structure
typedef struct {
    schedule_time_t time;
    char * command;
} schedule_entry2_t;


typedef struct {
	uint8_t WIFI_enable; 
	char * WIFI_ssid;
	char * WIFI_pass;
	uint8_t WIFI_DHCP;
	char *WIFI_ipAdress;
	char *WIFI_netMask;
	char *WIFI_gateWay;
	uint8_t WIFI_channel;
	
	uint8_t LAN_enable;
	uint8_t LAN_DHCP;
	char *LAN_ipAdress;
	char *LAN_netMask;
	char *LAN_gateWay;

	char *deviceName;
	int logLevel;
	long logMaxSize;
	int  logChapters;
	int  statusAllChannels;
	int  statusPeriod;
	uint8_t USB_debug;

	uint8_t FTP_enable;
	uint8_t FTP_anon;
	char *FTP_login;
	char *FTP_pass;

    char *udpServerAdress;
    uint16_t udpServerPort;
    uint16_t udpMyPort;
    char *udp_cross_link;

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

	char *				ntpServer;
	uint8_t 			scheduleCount;
	schedule_entry2_t 	scheduleEntries[MAX_SCHEDULE_ENTRIES];

} configuration;


uint8_t loadConfig(void);
FRESULT scan_in_dir(const char *file_extension, FF_DIR *dp, FILINFO *fno);
void load_Default_Config(void);
uint8_t loadContent(void);
int saveConfig(void);

void debugTopicLists(void);
uint8_t scanFileSystem();
uint8_t scan_dir(const char *path);

#define EVERY_SLOT		-1
#define waitForWorkPermit(a) waitForWorkPermit_((a), __FUNCTION__)
#define workIsPermitted(a) workIsPermitted_((a), __FUNCTION__)

void initWorkPermissions();
int workIsPermitted_(int slot_num, const char * moduleName);
void waitForWorkPermit_(int slot_num, const char * moduleName);
void setWorkPermission(int slot_num);
uint32_t xQueueReceiveLast(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait);

void makeStatusReport(bool spread);

