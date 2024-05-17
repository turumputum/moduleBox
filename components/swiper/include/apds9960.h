/*******************************************************************************
 * ----------------------------------------------------------------------------*
 *  elektronikaembedded@gamil.com ,https://elektronikaembedded.wordpress.com   *
 * ----------------------------------------------------------------------------*
 *                                                                             *
 * File Name  : apds9960.h                                                     *
 *                                                                             *
 * Description : APDS9960 IR Gesture Driver(Library for the SparkFun APDS-9960 breakout board)*
 *               SparkFun_APDS-9960.cpp Modified apds9960.c                    *
 * Version     : PrototypeV1.0                                                 *
 *                                                                             *
 * --------------------------------------------------------------------------- *
 * Authors: Sarath S (Modified Shawn Hymel (SparkFun Electronics))             *
 * Date: May 16, 2017                                                          *
 ******************************************************************************/

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __APDS9960_H
#define __APDS9960_H

/* Includes ------------------------------------------------------------------*/
/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/
/* Exported macro ------------------------------------------------------------*/

/* Debug */
#include "stdlib.h"
#include "stdio.h"
#include "esp_log.h"
#include "driver/i2c.h"

typedef struct {
	uint8_t dir;
	uint8_t speed;
}gesture_typeDef;

#define DEBUG                  0

/* APDS-9960 I2C address */
#define APDS9960_I2C_ADDR       0x39

/* Gesture parameters */
#define GESTURE_ARRAY_SIZE 250
#define GESTURE_THRESHOLD 60


/* Error code for returned values */
#define ERROR                   0xFF

//#define I2C_ADDRESS                                              0x5A
//#define I2C_ID_ADDRESS                                           0x5D
//#define I2C_TIMEOUT                                              10

#define FAIL -1
#define UP 0
#define DOWN 1
#define LEFT 2
#define RIGHT 3

#define POS 0
#define VAL 1


/* APDS-9960 register addresses */
#define APDS9960_ENABLE         0x80
#define APDS9960_ATIME          0x81
#define APDS9960_WTIME          0x83
#define APDS9960_AILTL          0x84
#define APDS9960_AILTH          0x85
#define APDS9960_AIHTL          0x86
#define APDS9960_AIHTH          0x87
#define APDS9960_PILT           0x89
#define APDS9960_PIHT           0x8B
#define APDS9960_PERS           0x8C
#define APDS9960_CONFIG1        0x8D
#define APDS9960_PPULSE         0x8E
#define APDS9960_CONTROL        0x8F
#define APDS9960_CONFIG2        0x90
#define APDS9960_ID             0x92
#define APDS9960_STATUS         0x93
#define APDS9960_CDATAL         0x94
#define APDS9960_CDATAH         0x95
#define APDS9960_RDATAL         0x96
#define APDS9960_RDATAH         0x97
#define APDS9960_GDATAL         0x98
#define APDS9960_GDATAH         0x99
#define APDS9960_BDATAL         0x9A
#define APDS9960_BDATAH         0x9B
#define APDS9960_PDATA          0x9C
#define APDS9960_POFFSET_UR     0x9D
#define APDS9960_POFFSET_DL     0x9E
#define APDS9960_CONFIG3        0x9F
#define APDS9960_GPENTH         0xA0
#define APDS9960_GEXTH          0xA1
#define APDS9960_GCONF1         0xA2
#define APDS9960_GCONF2         0xA3
#define APDS9960_GOFFSET_U      0xA4
#define APDS9960_GOFFSET_D      0xA5
#define APDS9960_GOFFSET_L      0xA7
#define APDS9960_GOFFSET_R      0xA9
#define APDS9960_GPULSE         0xA6
#define APDS9960_GCONF3         0xAA
#define APDS9960_GCONF4         0xAB
#define APDS9960_GFLVL          0xAE
#define APDS9960_GSTATUS        0xAF
#define APDS9960_IFORCE         0xE4
#define APDS9960_PICLEAR        0xE5
#define APDS9960_CICLEAR        0xE6
#define APDS9960_AICLEAR        0xE7
#define APDS9960_GFIFO_U        0xFC
#define APDS9960_GFIFO_D        0xFD
#define APDS9960_GFIFO_L        0xFE
#define APDS9960_GFIFO_R        0xFF

/* Bit fields */
#define APDS9960_PON            0x01
#define APDS9960_AEN            0x02
#define APDS9960_PEN            0x04
#define APDS9960_WEN            0x08
#define APSD9960_AIEN           0x10
#define APDS9960_PIEN           0x20
#define APDS9960_GEN            0x40
#define APDS9960_GVALID         0x01

/* On/Off definitions */
#define OFF                     0
#define ON                      1

/* Acceptable parameters for setMode */
#define POWER                   0
#define AMBIENT_LIGHT           1
#define PROXIMITY               2
#define WAIT                    3
#define AMBIENT_LIGHT_INT       4
#define PROXIMITY_INT           5
#define GESTURE                 6
#define ALL                     7

/* LED Drive values */
#define LED_DRIVE_100MA         0
#define LED_DRIVE_50MA          1
#define LED_DRIVE_25MA          2
#define LED_DRIVE_12_5MA        3

/* Proximity Gain (PGAIN) values */
#define PGAIN_1X                0
#define PGAIN_2X                1
#define PGAIN_4X                2
#define PGAIN_8X                3

/* ALS Gain (AGAIN) values */
#define AGAIN_1X                0
#define AGAIN_4X                1
#define AGAIN_16X               2
#define AGAIN_64X               3

/* Gesture Gain (GGAIN) values */
#define GGAIN_1X                0
#define GGAIN_2X                1
#define GGAIN_4X                2
#define GGAIN_8X                3

/* LED Boost values */
#define LED_BOOST_100           0
#define LED_BOOST_150           1
#define LED_BOOST_200           2
#define LED_BOOST_300           3    

/* Gesture wait time values */
#define GWTIME_0MS              0
#define GWTIME_2_8MS            1
#define GWTIME_5_6MS            2
#define GWTIME_8_4MS            3
#define GWTIME_14_0MS           4
#define GWTIME_22_4MS           5
#define GWTIME_30_8MS           6
#define GWTIME_39_2MS           7

/* Default values */
#define DEFAULT_ATIME           250    // 103ms
#define DEFAULT_WTIME           255     // 27ms
#define DEFAULT_PROX_PPULSE     0x87    // 16us, 8 pulses
#define DEFAULT_GESTURE_PPULSE  0x49   //0x89 // 16us, 10 pulses
#define DEFAULT_POFFSET_UR      0       // 0 offset
#define DEFAULT_POFFSET_DL      0       // 0 offset      
#define DEFAULT_CONFIG1         0x60    // No 12x wait (WTIME) factor
#define DEFAULT_LDRIVE          LED_DRIVE_100MA
#define DEFAULT_PGAIN           PGAIN_4X
#define DEFAULT_AGAIN           AGAIN_4X
#define DEFAULT_PILT            0       // Low proximity threshold
#define DEFAULT_PIHT            50      // High proximity threshold
#define DEFAULT_AILT            0xFFFF  // Force interrupt for calibration
#define DEFAULT_AIHT            0
#define DEFAULT_PERS            0x11    // 2 consecutive prox or ALS for int.
#define DEFAULT_CONFIG2         0x01    // No saturation interrupts or LED boost  
#define DEFAULT_CONFIG3         0       // Enable all photodiodes, no SAI
#define DEFAULT_GPENTH          0//40      // Threshold for entering gesture mode
#define DEFAULT_GEXTH           0//30      // Threshold for exiting gesture mode
#define DEFAULT_GCONF1          0x40//0x40    // 4 gesture events for int., 1 for exit
#define DEFAULT_GGAIN           GGAIN_8X//GGAIN_4X
#define DEFAULT_GLDRIVE         LED_DRIVE_100MA//LED_DRIVE_100MA
#define DEFAULT_GWTIME          GWTIME_0MS
#define DEFAULT_GOFFSET         0       // No offset scaling for gesture mode
#define DEFAULT_GPULSE_LEN      0    // 0 - 4us; 1 - 8us; 2 - 16us; 3-32us
#define DEFAULT_GPULSE_COUNT    30//25  //1-64
#define DEFAULT_GCONF3          0       // All photodiodes active during gesture
#define DEFAULT_GIEN            0       // Disable gesture interrupts
#define DEFAULT_LED_BOOST       LED_BOOST_100


#define WAITING 0
#define GESTURE_START 1
#define GESTURE_END 2

#define SWIPE_DOWN 1
#define SWIPE_UP 2
#define SWIPE_LEFT 3
#define SWIPE_RIGHT 4

#define NOT_RECOGNIZED 5


typedef struct gesture_data_type {
	uint8_t u_max;
	uint8_t d_max;
	uint8_t l_max;
	uint8_t r_max;

	uint8_t u_mid_val;
	uint8_t d_mid_val;
	uint8_t l_mid_val;
	uint8_t r_mid_val;

	int16_t u_upFront_pos; // [time, val]
	int16_t d_upFront_pos;
	int16_t l_upFront_pos;
	int16_t r_upFront_pos;

	int16_t u_downFront_pos;
	int16_t d_downFront_pos;
	int16_t l_downFront_pos;
	int16_t r_downFront_pos;

	int8_t u_d_rise_delta;
	int8_t u_d_fall_delta;
	int8_t r_l_rise_delta;
	int8_t r_l_fall_delta;

	int8_t vertical_delta;
	int8_t horizontal_delta;

	uint8_t size;
	uint64_t start_time;
	uint32_t duration;
	uint8_t gesture;
	
	uint8_t gestureMass[4][GESTURE_ARRAY_SIZE];

	uint8_t state;

} gesture_data_type;

//uint8_t i2c1_read(uint8_t memAdr, uint8_t *regData, uint8_t lenght);
esp_err_t i2c1_read(i2c_port_t  i2cPort, uint8_t reg_addr, uint8_t *data, uint8_t lenght);
//uint8_t i2c1_write(uint8_t memAdr, uint8_t regData);
esp_err_t  i2c1_write(i2c_port_t  i2cPort, uint8_t reg_addr, uint8_t data);

void setGpulseConfig(i2c_port_t i2c_port, uint8_t glen, uint8_t gpulse);
esp_err_t readSensor(i2c_port_t i2c_port, gesture_data_type *gData);
// void calibrateSensor(i2c_port_t i2c_port);

/* Exported functions ------------------------------------------------------- */

//uint8_t getProximity();

//esp_err_t apds9960init(i2c_port_t i2c_port) ;

esp_err_t setMode(i2c_port_t i2c_port, uint8_t mode, uint8_t enable);


uint8_t getMode(i2c_port_t i2c_port);


esp_err_t enablePower(i2c_port_t i2c_port);


esp_err_t enableGestureSensor(i2c_port_t i2c_port,int interrupts);


esp_err_t setLEDDrive(i2c_port_t i2c_port,uint8_t drive);


esp_err_t setGestureLEDDrive(i2c_port_t i2c_port,uint8_t drive);


esp_err_t setAmbientLightGain(i2c_port_t i2c_port,uint8_t drive);

esp_err_t setProximityGain(i2c_port_t i2c_port,uint8_t drive);


esp_err_t setGestureGain(i2c_port_t i2c_port,uint8_t gain);


esp_err_t setLightIntLowThreshold(i2c_port_t i2c_port,uint16_t threshold);


esp_err_t setLightIntHighThreshold(i2c_port_t i2c_port,uint16_t threshold);


esp_err_t setGestureIntEnable(i2c_port_t i2c_port,uint8_t enable);

//void readGesture(i2c_port_t i2c_port);

//int decodeGesture(i2c_port_t i2c_port);

// void resetGestureParameters(i2c_port_t i2c_port);

esp_err_t processGestureData(i2c_port_t i2c_port);

esp_err_t setProxIntHighThresh(i2c_port_t i2c_port, uint8_t threshold);

esp_err_t setProxIntLowThresh(i2c_port_t i2c_port, uint8_t threshold);

esp_err_t setLEDBoost(i2c_port_t i2c_port, uint8_t boost);

esp_err_t setGestureEnterThresh(i2c_port_t i2c_port, uint8_t threshold);

esp_err_t setGestureExitThresh(i2c_port_t i2c_port, uint8_t threshold);

esp_err_t setGestureWaitTime(i2c_port_t i2c_port, uint8_t time);

esp_err_t setGestureMode(i2c_port_t i2c_port, uint8_t mode);

// int apds9960ReadSensor(i2c_port_t i2c_port);

esp_err_t processGesture(gesture_data_type *gData);

void sensor_start(i2c_port_t i2c_port);

#endif /* __APDS9960_H */

/******* https://elektronikaembedded.wordpress.com  *****END OF FILE***********/
