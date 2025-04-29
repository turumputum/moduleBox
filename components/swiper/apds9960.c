/*******************************************************************************
 * ----------------------------------------------------------------------------*
 *  elektronikaembedded@gamil.com ,https://elektronikaembedded.wordpress.com   *
 * ----------------------------------------------------------------------------*
 *                                                                             *
 * File Name  : apds9960.c                                                     *
 *                                                                             *
 * Description : APDS9960 IR Gesture Driver(Library for the SparkFun APDS-9960 breakout board)*
 *               SparkFun_APDS-9960.cpp Modified apds9960.c                    *
 * Version     : PrototypeV1.0                                                 *
 *                                                                             *
 * --------------------------------------------------------------------------- *
 * Authors: Sarath S (Modified Shawn Hymel (SparkFun Electronics))             *
 * Date: May 16, 2017                                                          *
 ******************************************************************************/

/* Includes ------------------------------------------------------------------*/
/* MCU Files */

/*Std Library Files */
#include "stdlib.h"
/* User Files */
#include "apds9960.h"
#include "stdio.h"
#include "string.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "driver/gpio.h"

#include "esp_timer.h"

#include "sdkconfig.h"

#include "reporter.h"
#include "stateConfig.h"


/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/


//extern int tempRight, tempLeft;
// uint8_t raw_up, raw_down, raw_left, raw_right;
// uint8_t fifo_size;

// uint8_t flag_gesture_reading;
// uint8_t flag_gesture_start = 0;
// uint8_t flag_gesture_end = 0;

// uint32_t curent_gesture_size = 0;


// int up_offset = 0;
// int down_offset = 0;
// int left_offset = 0;
// int right_offset = 0;

// uint8_t sensorAdr;

//gesture_data_type Gdata;


// uint8_t requestSend = 0;
// uint8_t dataAvailable = 0;

extern uint8_t SLOTS_PIN_MAP[10][4];
extern configuration me_config;
extern stateStruct me_state;

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "SWIPER";


#define I2C_MASTER_TIMEOUT_MS       1000

/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/


esp_err_t i2c1_read(i2c_port_t  i2cPort, uint8_t reg_addr, uint8_t *data, uint8_t lenght) {
	
	//if (HAL_I2C_Mem_Read(bus, sensorAdr, memAdr, 1, regData, lenght,HAL_MAX_DELAY) == ESP_OK) {
	return i2c_master_write_read_device(i2cPort, APDS9960_I2C_ADDR, &reg_addr, 1, data, lenght, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

}

esp_err_t  i2c1_write(i2c_port_t  i2cPort, uint8_t reg_addr, uint8_t data) {
	//if (HAL_I2C_Mem_Write(&hi2c1, sensorAdr, memAdr, 1, &regData, 1, HAL_MAX_DELAY) == ESP_OK) {
	esp_err_t ret;
    uint8_t write_buf[2] = {reg_addr, data};

    ret = i2c_master_write_to_device(i2cPort, APDS9960_I2C_ADDR, write_buf, sizeof(write_buf), I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

    return ret;
}

// void debugPutChar(char ch) {
// 	ESP_LOGD(TAG,"%d", ch);
// }

// void debugPutString(char *ch) {
// 	ESP_LOGD(TAG,"%s", ch);
// }

uint8_t getProximity(i2c_port_t i2c_port) {
	uint8_t prox;
	i2c1_read(i2c_port,APDS9960_PDATA, &prox, 1);
	return prox;
}


esp_err_t disableVerticalAxis(i2c_port_t i2c_port)
{
    esp_err_t ret;
    uint8_t val;

    // Считываем текущее значение регистра GCONF3
    ret = i2c1_read(i2c_port, APDS9960_GCONF3, &val, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    // Очищаем младшие два бита и устанавливаем GDIMS = 0b10 (только горизонтальные жесты)
    val &= 0xFC;   // 0xFC = 11111100b — сбрасывает биты [1:0]
    val |= 0x02;   // 0x02 = 00000010b — включает только L/R

    // Записываем обновленное значение обратно в GCONF3
    ret = i2c1_write(i2c_port, APDS9960_GCONF3, val);
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}



/* ----------------------------------------------------------------------------*
 *
 *  Function Name : apds9960init
 *
 *  Description  : Configures I2C communications and initializes registers to defaults
 *
 *  Input : None
 *
 *  Output : None
 *
 *  Return : 1 if initialized successfully. 0 otherwise
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t apds9960init(i2c_port_t i2c_port) {
	uint8_t id;
	//ledSetLeftLed(LED_ON);

	/* Initialize I2C */
	if(DEBUG){
		ESP_LOGD(TAG,"Init gesture sensor start  ");
	}

	/* Read ID register and check against known values for APDS-9960 */
	if (i2c1_read(i2c_port,APDS9960_ID, &id, 1)!=ESP_OK) {
		ESP_LOGE(TAG,"read sensor ID failed at %d ", id);
	}else{
		if(DEBUG){
			ESP_LOGD(TAG,"Sensor ID is %d ", id);
		}
	}

	/* Set ENABLE register to 0 (disable all features) */
	if (setMode(i2c_port, ALL, OFF)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Set default values for ambient light and proximity registers */
	if (i2c1_write(i2c_port,APDS9960_ATIME, DEFAULT_ATIME)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_WTIME, DEFAULT_WTIME)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_PPULSE, DEFAULT_PROX_PPULSE)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_POFFSET_UR, DEFAULT_POFFSET_UR)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_POFFSET_DL, DEFAULT_POFFSET_DL)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_CONFIG1, DEFAULT_CONFIG1)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setLEDDrive(i2c_port, DEFAULT_LDRIVE)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setProximityGain(i2c_port, DEFAULT_PGAIN)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setAmbientLightGain(i2c_port, DEFAULT_AGAIN)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setProxIntLowThresh(i2c_port, DEFAULT_PILT)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setProxIntHighThresh(i2c_port, DEFAULT_PIHT)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setLightIntLowThreshold(i2c_port, DEFAULT_AILT)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setLightIntHighThreshold(i2c_port, DEFAULT_AIHT)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_PERS, DEFAULT_PERS)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_CONFIG2, DEFAULT_CONFIG2)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_CONFIG3, DEFAULT_CONFIG3)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Set default values for gesture sense registers */
	if (setGestureEnterThresh(i2c_port, DEFAULT_GPENTH)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setGestureExitThresh(i2c_port, DEFAULT_GEXTH)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_GCONF1, DEFAULT_GCONF1)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setGestureGain(i2c_port, DEFAULT_GGAIN)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setGestureLEDDrive(i2c_port, DEFAULT_GLDRIVE)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setGestureWaitTime(i2c_port, DEFAULT_GWTIME)!=ESP_OK) {
		return ESP_FAIL;
	}

	if (i2c1_write(i2c_port,APDS9960_GOFFSET_U, DEFAULT_GOFFSET)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_GOFFSET_D, DEFAULT_GOFFSET)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_GOFFSET_L, DEFAULT_GOFFSET)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_GOFFSET_R, DEFAULT_GOFFSET)!=ESP_OK) {
		return ESP_FAIL;
	}

	setGpulseConfig(i2c_port, DEFAULT_GPULSE_LEN, DEFAULT_GPULSE_COUNT);

	if (i2c1_write(i2c_port,APDS9960_GCONF3, DEFAULT_GCONF3)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setGestureIntEnable(i2c_port, DEFAULT_GIEN)!=ESP_OK) {
		return ESP_FAIL;
	}

	return ESP_OK;
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setMode(uint8_t mode, uint8_t enable)
 *
 *  Description  :Enables or disables a feature in the APDS-9960
 *
 *  Input : mode which feature to enable,enable ON (1) or OFF (0)
 *
 *  Output : None
 *
 *  Return : 1 if operation success. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setMode(i2c_port_t i2c_port, uint8_t mode, uint8_t enable) {
	uint8_t reg_val;

	/* Read current ENABLE register */
	reg_val = getMode(i2c_port);

	/* Change bit(s) in ENABLE register */
	enable = enable & 0x01;
	if (mode >= 0 && mode <= 6) {
		if (enable) {
			reg_val |= (1 << mode);
		} else {
			reg_val &= ~(1 << mode);
		}
	} else if (mode == ALL) {
		if (enable) {
			reg_val = 0x7F;
		} else {
			reg_val = 0x00;
		}
	}

	/* Write value back to ENABLE register */
	return i2c1_write(i2c_port,APDS9960_ENABLE, reg_val);

}/* End of this function */

void setGpulseConfig(i2c_port_t i2c_port, uint8_t glen, uint8_t gpulse) {
	uint8_t out = glen << 5;
	out = out | gpulse;

	if (i2c1_write(i2c_port,APDS9960_GPULSE, out)==ESP_OK) {
		ESP_LOGD(TAG,"set GPULSE reg: %d  ", out);
	}

}

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setMode(uint8_t mode, uint8_t enable)
 *
 *  Description  :Reads and returns the contents of the ENABLE register
 *
 *  Input : mode which feature to enable,enable ON (1) or OFF (0)
 *
 *  Output : None
 *
 *  Return : Contents of the ENABLE register. 0xFF if error.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
uint8_t getMode(i2c_port_t i2c_port) {
	uint8_t enable_value;

	/* Read current ENABLE register */
	if (i2c1_read(i2c_port,APDS9960_ENABLE, &enable_value, 1)!=ESP_OK) {
		return ESP_FAIL;
	}

	return enable_value;
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setLEDDrive(uint8_t drive)
 *
 *  Description  : Sets the LED drive strength for proximity and ALS
 *  Value    LED Current
 *   0        100 mA
 *   1         50 mA
 *   2         25 mA
 *   3         12.5 mA
 *  Input : drive the value (0-3) for the LED drive strength
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setLEDDrive(i2c_port_t i2c_port, uint8_t drive) {
	uint8_t val;

	/* Read value from CONTROL register */
	if (i2c1_read(i2c_port,APDS9960_CONTROL, &val, 1)!=0) {
		return ESP_FAIL;
	}

	/* Set bits in register to given value */
	drive &= 0x03;
	drive = drive << 6;
	val &= 0x3F;
	val |= drive;

	/* Write register value back into CONTROL register */
	return i2c1_write(i2c_port,APDS9960_CONTROL, val);
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setProximityGain(uint8_t drive)
 *
 *  Description  :Sets the receiver gain for proximity detection
 *  Value    Gain
 *   0       1x
 *   1       2x
 *   2       4x
 *   3       8x
 *  Input : drive the value (0-3) for the gain
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setProximityGain(i2c_port_t i2c_port, uint8_t drive) {
	uint8_t val;

	/* Read value from CONTROL register */
	if (i2c1_read(i2c_port,APDS9960_CONTROL, &val, 1)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Set bits in register to given value */
	drive &= 0x03;
	drive = drive << 2;
	val &= 0xF3;
	val |= drive;

	/* Write register value back into CONTROL register */
	return i2c1_write(i2c_port,APDS9960_CONTROL, val);
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setAmbientLightGain(uint8_t drive)
 *
 *  Description  :Sets the receiver gain for the ambient light sensor (ALS)
 *  Value    Gain
 *   0        1x
 *   1        4x
 *   2       16x
 *   3       64x
 *  Input : drive the value (0-3) for the gain
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setAmbientLightGain(i2c_port_t i2c_port, uint8_t drive) {
	uint8_t val;

	/* Read value from CONTROL register */
	if (i2c1_read(i2c_port,APDS9960_CONTROL, &val, 1)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Set bits in register to given value */
	drive &= 0x03;
	val &= 0xFC;
	val |= drive;

	/* Write register value back into CONTROL register */
	return i2c1_write(i2c_port,APDS9960_CONTROL, val);
}/* End of this function */
/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setProxIntLowThresh(uint8_t threshold)
 *
 *  Description  :Sets the lower threshold for proximity detection
 *
 *  Input : threshold the lower proximity threshold
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/

esp_err_t setProxIntLowThresh(i2c_port_t i2c_port, uint8_t threshold) {
	return i2c1_write(i2c_port,APDS9960_PILT, threshold);
}/* End of this function */
/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setProxIntHighThresh(uint8_t threshold)
 *
 *  Description  :Sets the high threshold for proximity detection
 *
 *  Input : threshold the high proximity threshold
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setProxIntHighThresh(i2c_port_t i2c_port, uint8_t threshold) {
	return i2c1_write(i2c_port,APDS9960_PIHT, threshold);
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setLightIntLowThreshold(uint16_t threshold)
 *
 *  Description  :Sets the low threshold for ambient light interrupts
 *
 *  Input : threshold low threshold value for interrupt to trigger
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setLightIntLowThreshold(i2c_port_t i2c_port, uint16_t threshold) {
	uint8_t val_low;
	uint8_t val_high;

	/* Break 16-bit threshold into 2 8-bit values */
	val_low = threshold & 0x00FF;
	val_high = (threshold & 0xFF00) >> 8;

	/* Write low byte */
	if (i2c1_write(i2c_port,APDS9960_AILTL, val_low)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Write high byte */
	if (i2c1_write(i2c_port,APDS9960_AILTH, val_high)!=ESP_OK){
		return ESP_FAIL;
	}

	return ESP_OK;
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setLightIntHighThreshold(uint16_t threshold)
 *
 *  Description  :Sets the high threshold for ambient light interrupts
 *
 *  Input : threshold high threshold value for interrupt to trigger
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setLightIntHighThreshold(i2c_port_t i2c_port, uint16_t threshold) {
	uint8_t val_low;
	uint8_t val_high;

	/* Break 16-bit threshold into 2 8-bit values */
	val_low = threshold & 0x00FF;
	val_high = (threshold & 0xFF00) >> 8;

	/* Write low byte */
	if (i2c1_write(i2c_port,APDS9960_AIHTL, val_low)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Write high byte */
	if (i2c1_write(i2c_port,APDS9960_AIHTH, val_high)!=ESP_OK) {
		return ESP_FAIL;
	}

	return ESP_OK;
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setGestureIntEnable(uint8_t enable)
 *
 *  Description  :Turns gesture-related interrupts on or off
 *
 *  Input : enable 1 to enable interrupts, 0 to turn them off
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setGestureIntEnable(i2c_port_t i2c_port, uint8_t enable) {
	uint8_t val;

	/* Read value from GCONF4 register */
	if (i2c1_read(i2c_port,APDS9960_GCONF4, &val, 1)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Set bits in register to given value */
	enable &= 0x01;
	enable = enable << 1;
	val &= 0xFD;
	val |= enable;

	/* Write register value back into GCONF4 register */
	if (i2c1_write(i2c_port,APDS9960_GCONF4, val)!=ESP_OK) {
		return ESP_FAIL;
	}

	return ESP_OK;
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setGestureWaitTime(uint8_t time)
 *
 *  Description  :Sets the time in low power mode between gesture detections
 *  Value    Wait time
 *   0          0 ms
 *   1          2.8 ms
 *   2          5.6 ms
 *   3          8.4 ms
 *   4         14.0 ms
 *   5         22.4 ms
 *   6         30.8 ms
 *   7         39.2 ms
 *  Input : the value for the wait time
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setGestureWaitTime(i2c_port_t i2c_port, uint8_t time) {
	uint8_t val;

	/* Read value from GCONF2 register */
	if (i2c1_read(i2c_port,APDS9960_GCONF2, &val, 1)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Set bits in register to given value */
	time &= 0x07;
	val &= 0xF8;
	val |= time;

	/* Write register value back into GCONF2 register */
	if (i2c1_write(i2c_port,APDS9960_GCONF2, val)!=ESP_OK) {
		return ESP_FAIL;
	}

	return ESP_OK;
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setGestureGain(uint8_t gain)
 *
 *  Description  :Sets the gain of the photodiode during gesture mode
 * Value    Gain
 *   0       1x
 *   1       2x
 *   2       4x
 *   3       8x
 *  Input : gain the value for the photodiode gain
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setGestureGain(i2c_port_t i2c_port, uint8_t gain) {
	uint8_t val;

	/* Read value from GCONF2 register */
	if (i2c1_read(i2c_port,APDS9960_GCONF2, &val, 1)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Set bits in register to given value */
	gain &= 0x03;
	gain = gain << 5;
	val &= 0x9F;
	val |= gain;

	/* Write register value back into GCONF2 register */
	if (i2c1_write(i2c_port,APDS9960_GCONF2, val)!=ESP_OK) {
		return ESP_FAIL;
	}

	return ESP_OK;
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setGestureExitThresh(uint8_t threshold)
 *
 *  Description  :Sets the exit proximity threshold for gesture sensing
 *
 *  Input : threshold proximity value needed to end gesture mode
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setGestureExitThresh(i2c_port_t i2c_port, uint8_t threshold) {
	return i2c1_write(i2c_port,APDS9960_GEXTH, threshold);
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : setGestureEnterThresh(uint8_t threshold)
 *
 *  Description  :Sets the entry proximity threshold for gesture sensing
 *
 *  Input : threshold proximity value needed to start gesture mode
 *
 *  Output : None
 *
 *  Return : 1 if operation successful. 0 otherwise.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t setGestureEnterThresh(i2c_port_t i2c_port, uint8_t threshold) {
	return i2c1_write(i2c_port,APDS9960_GPENTH, threshold);
}/* End of this function */

/* ----------------------------------------------------------------------------*
 *
 *  Function Name : enableGestureSensor(int interrupts)
 *
 *  Description  :Starts the gesture recognition engine on the APDS-9960
 *
 *  Input : interrupts 1 to enable hardware external interrupt on gesture
 *
 *  Output : None
 *
 *  Return : 1 if engine enabled correctly. 0 on error.
 * ----------------------------------------------------------------------------*
 * Authors: Sarath S
 * Date: May 17, 2017
 * ---------------------------------------------------------------------------*/
esp_err_t enableGestureSensor(i2c_port_t i2c_port, int interrupts) {

	/* Enable gesture mode
	 Set ENABLE to 0 (power off)
	 Set WTIME to 0xFF
	 Set AUX to LED_BOOST_300
	 Enable PON, WEN, PEN, GEN in ENABLE
	 */
	if (i2c1_write(i2c_port,APDS9960_WTIME, 0xFF)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (i2c1_write(i2c_port,APDS9960_PPULSE, DEFAULT_GESTURE_PPULSE)!=ESP_OK) {
		return ESP_FAIL;
	}
	//if (!setLEDBoost(LED_BOOST_300)) {
	//if (!setLEDBoost(DEFAULT_LED_BOOST)) {
	//return 0;
	//}
	if (interrupts) {
		if (setGestureIntEnable(i2c_port, 1)!=ESP_OK) {
			return ESP_FAIL;
		}
	} else {
		if (setGestureIntEnable(i2c_port,0)!=ESP_OK) {
			return ESP_FAIL;
		}
	}
	if (setGestureMode(i2c_port,1)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (enablePower(i2c_port)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setMode(i2c_port,WAIT, 0)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setMode(i2c_port,PROXIMITY, 0)!=ESP_OK) {
		return ESP_FAIL;
	}
	if (setMode(i2c_port,GESTURE, 1)!=ESP_OK) {
		return ESP_FAIL;
	}

	return ESP_OK;
}/* End of this function */

esp_err_t setGestureLEDDrive(i2c_port_t i2c_port, uint8_t drive) {
	uint8_t val;

	/* Read value from GCONF2 register */
	if (i2c1_read(i2c_port,APDS9960_GCONF2, &val, 1)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Set bits in register to given value */
	drive &= 0x03;
	drive = drive << 3;
	val &= 0xE7;
	val |= drive;

	/* Write register value back into GCONF2 register */
	return i2c1_write(i2c_port,APDS9960_GCONF2, val);
}/* End of this function */

esp_err_t setLEDBoost(i2c_port_t i2c_port, uint8_t boost) {
	uint8_t val;

	/* Read value from CONFIG2 register */
	if (i2c1_read(i2c_port,APDS9960_CONFIG2, &val, 1)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Set bits in register to given value */
	boost &= 0x03;
	boost = boost << 4;
	val &= 0xCF;
	val |= boost;

	/* Write register value back into CONFIG2 register */
	return i2c1_write(i2c_port,APDS9960_CONFIG2, val);
}/* End of this function */

esp_err_t setGestureMode(i2c_port_t i2c_port, uint8_t mode) {
	uint8_t val;

	/* Read value from GCONF4 register */
	if (i2c1_read(i2c_port,APDS9960_GCONF4, &val, 1)!=ESP_OK) {
		return ESP_FAIL;
	}

	/* Set bits in register to given value */
	mode &= 0x01;
	val &= 0xFE;
	val |= mode;

	/* Write register value back into GCONF4 register */
	return i2c1_write(i2c_port,APDS9960_GCONF4, val);
}/* End of this function */

esp_err_t enablePower(i2c_port_t i2c_port) {
	return setMode(i2c_port, POWER, 1);
}/* End of this function */

// void uartGesture() {
// 	char str_tx[60];

// 	if (flag_gesture_end) {
// 		flag_gesture_end = 0;

// 		if (curent_gesture_size > 3) {
// 			memset(str_tx, 0, strlen(str_tx));
// 			sESP_LOGD(TAG,str_tx, "START. Gesture size: %ld  ", curent_gesture_size);
// 			CDC_Transmit_FS((unsigned char*) str_tx, strlen(str_tx));
// 			HAL_Delay(1);

// 			for (int i = 0; i < curent_gesture_size; i++) {
// 				memset(str_tx, 0, strlen(str_tx));
// 				sESP_LOGD(TAG,str_tx, "% 5d, % 5d, % 5d, % 5d  ", gestureMass[UP][i], gestureMass[DOWN][i], gestureMass[LEFT][i], gestureMass[RIGHT][i]);
// 				CDC_Transmit_FS((unsigned char*) str_tx, strlen(str_tx));
// 				HAL_Delay(1);
// 			}

// 			memset(str_tx, 0, strlen(str_tx));
// 			sESP_LOGD(TAG,str_tx, "STOP  ");
// 			CDC_Transmit_FS((unsigned char*) str_tx, strlen(str_tx));
// 		}

// 	}
// }

void setOssfset(i2c_port_t i2c_port, int dir, int8_t offset) {

	if (offset < 0) {
		offset = 128 + abs(offset);
	}

	i2c1_write(i2c_port,dir, offset);
}

void calibrateSensor(i2c_port_t i2c_port) {

#define CALIBRATE_THRESHOLD_MIN 10
#define CALIBRATE_THRESHOLD_MAX 20
#define CALIBRATE_SIZE 30

	int i;
	uint8_t fifo_data[128];
	uint8_t fifo_level = 0;
	int bytes_read = 0;
	uint8_t calibrated[4] = { 0, };

	int8_t upOff = 0;
	int8_t downOff = 0;
	int8_t leftOff = 0;
	int8_t rightOff = 0;

	uint16_t tmp_up = 0;
	uint16_t tmp_down = 0;
	uint16_t tmp_left = 0;
	uint16_t tmp_right = 0;
	uint16_t currentMassSize = 0;

	if(DEBUG){
		ESP_LOGD(TAG,"Start calibration.");
	}

	while ((calibrated[UP]!=1)||(calibrated[DOWN]!=1)||(calibrated[LEFT]!=1)||(calibrated[RIGHT]!=1)) {
		vTaskDelay(pdMS_TO_TICKS(100));
		if (i2c1_read(i2c_port, APDS9960_GFLVL, &fifo_level, 1)!=ESP_OK) {
			ESP_LOGD(TAG,"FIFO read error  ");
			//flagSensorReset = 1;
		}

		if (fifo_level > 0) {
			//ESP_LOGD(TAG,"FIFO Level:%d  ", fifo_level);
			//i2c_master_read_byte()
			bytes_read = fifo_level * 4;
			esp_err_t err = i2c1_read(i2c_port, APDS9960_GFIFO_U, fifo_data, (fifo_level * 4));
			if(err!=ESP_OK){
				ESP_LOGD(TAG,"FIFO Fifo read error: %s", esp_err_to_name(err));
			}else {
				/* If at least 1 set of data, sort the data into U/D/L/R */
				for (i = 0; i < bytes_read; i += 4) {
					tmp_up += fifo_data[i + 0];
					tmp_down += fifo_data[i + 1];
					tmp_left += fifo_data[i + 2];
					tmp_right += fifo_data[i + 3];
					currentMassSize++;
				}
			}

			if (currentMassSize > CALIBRATE_SIZE) {

				tmp_up = tmp_up / currentMassSize;
				tmp_down = tmp_down / currentMassSize;
				tmp_left = tmp_left / currentMassSize;
				tmp_right = tmp_right / currentMassSize;

				if(DEBUG){
					ESP_LOGD(TAG,"calibration data: up %d down %d left %d right %d  ", tmp_up, tmp_down, tmp_left, tmp_right);
				}

				if (calibrated[UP] == 0) {
					if (tmp_up > CALIBRATE_THRESHOLD_MAX) {
						upOff++;
						if (upOff > 125)
							upOff = 125;
					} else if (tmp_up < CALIBRATE_THRESHOLD_MIN) {
						upOff--;
						if (upOff < -125)
							upOff = -125;
					} else {
						calibrated[UP] = 1;
					}
				}

				if (calibrated[DOWN] == 0) {
					if (tmp_down > CALIBRATE_THRESHOLD_MAX) {
						downOff++;
						if (downOff > 125)
							downOff = 125;
					} else if (tmp_down < CALIBRATE_THRESHOLD_MIN) {
						downOff--;
						if (downOff < -125)
							downOff = -125;
					} else {
						calibrated[DOWN] = 1;
					}
				}

				if (calibrated[LEFT] == 0) {
					if (tmp_left > CALIBRATE_THRESHOLD_MAX) {
						leftOff++;
						if (leftOff > 125)
							leftOff = 125;
					} else if (tmp_left < CALIBRATE_THRESHOLD_MIN) {
						leftOff--;
						if (leftOff < -125)
							leftOff = -125;
					} else {
						calibrated[LEFT] = 1;
					}
				}

				if (calibrated[RIGHT] == 0) {
					if (tmp_right > CALIBRATE_THRESHOLD_MAX) {
						rightOff++;
						if (rightOff > 125)
							rightOff = 125;
					} else if (tmp_right < CALIBRATE_THRESHOLD_MIN) {
						rightOff--;
						if (rightOff < -125)
							rightOff = -125;
					} else {
						calibrated[RIGHT] = 1;
					}
				}

				setOssfset(i2c_port,APDS9960_GOFFSET_U, upOff);
				setOssfset(i2c_port,APDS9960_GOFFSET_D, downOff);
				setOssfset(i2c_port,APDS9960_GOFFSET_L, leftOff);
				setOssfset(i2c_port,APDS9960_GOFFSET_R, rightOff);

				if(DEBUG){
					ESP_LOGD(TAG,"offset state: up %d down %d left %d right %d", upOff, downOff, leftOff, rightOff);
				}

				tmp_up = 0;
				tmp_down = 0;
				tmp_left = 0;
				tmp_right = 0;
				currentMassSize = 0;

			}

		}
	}

	ESP_LOGD(TAG,"Calibration end. UP_offset:%d DOWN_offset:%d LEFT_offset:%d RIGH_offset:%d", upOff, downOff, leftOff, rightOff);

}

void sensor_start(i2c_port_t i2c_port) {
	if (apds9960init(i2c_port)==ESP_OK) {
		ESP_LOGD(TAG,"APDS-9960 initialization complete ");
	} else {
		ESP_LOGE(TAG,"Something went wrong during APDS-9960 init! ");
	}
	// Start running the APDS-9960 gesture sensor engine
	if (enableGestureSensor(i2c_port, 1)==ESP_OK) {
		ESP_LOGD(TAG,"Gesture sensor is now running ");
	} else {
		ESP_LOGE(TAG,"Something went wrong during gesture sensor init! ");
	}

	calibrateSensor(i2c_port);
}

int chekVal(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
	uint8_t e = GESTURE_THRESHOLD;

	if ((a > e) || (b > e) || (d > e) || (d > e)) {
		return 1;
	}

	return 0;
}

void endOfGesture(gesture_data_type *gData) {
	//Gdata.size = curent_gesture_size;
	//gData->time = esp_timer_get_time() - gData->time;
	gData->duration = esp_timer_get_time() - gData->start_time;
	gData->state = GESTURE_END;
	if(DEBUG){
		ESP_LOGD(TAG,"Gesture end, time:%llu duratuin:%ld  ", esp_timer_get_time(), gData->duration);
	}
}

void resetGesture(gesture_data_type *gData) {
	for (int i = 0; i < GESTURE_ARRAY_SIZE; i++) {
		gData->gestureMass[UP][i] = 0;
		gData->gestureMass[DOWN][i] = 0;
		gData->gestureMass[LEFT][i] = 0;
		gData->gestureMass[RIGHT][i] = 0;
	}
	gData->size = 0;
	gData->start_time = esp_timer_get_time();
}

esp_err_t readSensor(i2c_port_t i2c_port, gesture_data_type *gData) {

	//uint32_t functionStartTick = esp_timer_get_time();
	int i;
	uint8_t fifo_data[128];
	uint8_t fifo_level = 0;
	int bytes_read = 0;
	uint8_t tmp_up, tmp_down, tmp_left, tmp_right;
	uint8_t gStatus=0;

	if (i2c1_read(i2c_port,APDS9960_GSTATUS, &gStatus, 1)!= ESP_OK) {
		ESP_LOGE(TAG,"Status read error  ");
		return ESP_FAIL;
	}
	if((gStatus & 0b00000010)>0){
		ESP_LOGE(TAG,"FIFO overflow");
	}

	if (i2c1_read(i2c_port,APDS9960_GFLVL, &fifo_level, 1)!= ESP_OK) {
		ESP_LOGE(TAG,"FIFO read error  ");
		return ESP_FAIL;
	}

	if ((fifo_level > 0)&&((gStatus & 0b00000001)>0)) {
		//ESP_LOGD(TAG,"FIFO Level:%d  ", fifo_level);
		bytes_read = (fifo_level * 4);
		if(i2c1_read(i2c_port,APDS9960_GFIFO_U, fifo_data, (fifo_level * 4))!=ESP_OK){
			ESP_LOGE(TAG,"Mem read fail ");
			return ESP_FAIL;
		}

		if (bytes_read >= 4) {
			for (i = 0; i < bytes_read; i += 4) {
				tmp_up = fifo_data[i + 0];
				tmp_down = fifo_data[i + 1];
				tmp_left = fifo_data[i + 2];
				tmp_right = fifo_data[i + 3];
				//ESP_LOGD(TAG,"  %ld : %d, %d, %d, %d   size:%lu ", esp_timer_get_time(), tmp_up, tmp_down, tmp_left, tmp_right, curent_gesture_size);
				if (chekVal(tmp_up, tmp_down, tmp_left, tmp_right)) {
					if (gData->state == WAITING) {
						gData->state = GESTURE_START;
						resetGesture(gData);
						if(DEBUG){
							ESP_LOGD(TAG,"Gesture start! at levels: %d, %d, %d, %d ",  tmp_up, tmp_down, tmp_left, tmp_right);
							//ESP_LOGD(TAG,"  ");
						}
					}

					gData->gestureMass[UP][gData->size] = tmp_up;
					gData->gestureMass[DOWN][gData->size] = tmp_down;
					gData->gestureMass[LEFT][gData->size] = tmp_left;
					gData->gestureMass[RIGHT][gData->size] = tmp_right;
					gData->size++;
					if(DEBUG){
						//ESP_LOGD(TAG,"  %lld : %d, %d, %d, %d   size:%d ", esp_timer_get_time(), tmp_up, tmp_down, tmp_left, tmp_right, gData->size);
					}
					if (gData->size > GESTURE_ARRAY_SIZE-2) {
						ESP_LOGD(TAG,"  sooo looong gesture  ");
						endOfGesture(gData);
						return ESP_OK;
					}

				} else if (gData->state == GESTURE_START) {
					if(DEBUG){
						ESP_LOGD(TAG,"end of gesture: %d, %d, %d, %d   size:%d ",tmp_up, tmp_down, tmp_left, tmp_right, gData->size);
					}
					endOfGesture(gData);
					return ESP_OK;
				}

			}
		}
	}
	return ESP_OK;
	//ESP_LOGD(TAG,"read sensor time: %lu  ", esp_timer_get_time() - functionStartTick);
}

void searhchMaxMidVal(gesture_data_type *gData) {
	uint32_t startTick = esp_timer_get_time();

	gData->u_max = 0;
	gData->d_max = 0;
	gData->l_max = 0;
	gData->r_max = 0;

	for (int i = 0; i < gData->size; i++) {
		if (gData->u_max < gData->gestureMass[UP][i]) {
			gData->u_max = gData->gestureMass[UP][i];
		}
		if (gData->d_max < gData->gestureMass[DOWN][i]) {
			gData->d_max = gData->gestureMass[DOWN][i];
		}
		if (gData->l_max < gData->gestureMass[LEFT][i]) {
			gData->l_max = gData->gestureMass[LEFT][i];
		}
		if (gData->r_max < gData->gestureMass[RIGHT][i]) {
			gData->r_max = gData->gestureMass[RIGHT][i];
		}
	}

	gData->u_mid_val = gData->u_max / 2;
	gData->d_mid_val = gData->d_max / 2;
	gData->l_mid_val = gData->l_max / 2;
	gData->r_mid_val = gData->r_max / 2;

	if (DEBUG) {
		ESP_LOGD(TAG,"  Search midle and max val end, duration:%llu  MID val's UP:%d DOWM:%d LEFT:%d RIGHT:%d  ", esp_timer_get_time() - startTick, gData->u_mid_val, gData->d_mid_val, gData->l_mid_val, gData->r_mid_val);
	}
}

void searhchFrontIntersectPos(gesture_data_type *gData) {
	uint32_t startTick = esp_timer_get_time();

	gData->u_upFront_pos = -1;
	gData->d_upFront_pos = -1;
	gData->l_upFront_pos = -1;
	gData->r_upFront_pos = -1;
	uint8_t count = 0;

	for (int i = 0; i < gData->size; i++) {
		if ((gData->gestureMass[UP][i] > gData->u_mid_val) && (gData->u_upFront_pos == -1)) {
			gData->u_upFront_pos = i;
			count++;
		}
		if ((gData->gestureMass[DOWN][i] > gData->d_mid_val) && (gData->d_upFront_pos == -1)) {
			gData->d_upFront_pos = i;
			count++;
		}
		if ((gData->gestureMass[LEFT][i] > gData->l_mid_val) && (gData->l_upFront_pos == -1)) {
			gData->l_upFront_pos = i;
			count++;
		}
		if ((gData->gestureMass[RIGHT][i] > gData->r_mid_val) && (gData->r_upFront_pos == -1)) {
			gData->r_upFront_pos = i;
			count++;
		}

		if (count >= 4) {
			break;
		}
	}

	gData->u_downFront_pos = -1;
	gData->d_downFront_pos = -1;
	gData->l_downFront_pos = -1;
	gData->r_downFront_pos = -1;
	count = 0;

	for (int i = gData->size - 1; i > 0; i--) {
		if ((gData->gestureMass[UP][i] > gData->u_mid_val) && (gData->u_downFront_pos == -1)) {
			gData->u_downFront_pos = i;
			count++;
		}
		if ((gData->gestureMass[DOWN][i] > gData->d_mid_val) && (gData->d_downFront_pos == -1)) {
			gData->d_downFront_pos = i;
			count++;
		}
		if ((gData->gestureMass[LEFT][i] > gData->l_mid_val) && (gData->l_downFront_pos == -1)) {
			gData->l_downFront_pos = i;
			count++;
		}
		if ((gData->gestureMass[RIGHT][i] > gData->r_mid_val) && (gData->r_downFront_pos == -1)) {
			gData->r_downFront_pos = i;
			count++;
		}

		if (count >= 4) {
			break;
		}
	}

	if (DEBUG) {
		ESP_LOGD(TAG,"  Search fronts, duration:%llu  UP/DOWN val's UP:%d/%d DOWM:%d/%d LEFT:%d/%d RIGHT:%d/%d  ", esp_timer_get_time() - startTick, gData->u_upFront_pos,
				gData->u_downFront_pos, gData->d_upFront_pos, gData->d_downFront_pos, gData->l_upFront_pos, gData->l_downFront_pos, gData->r_upFront_pos, gData->r_downFront_pos);
	}

}

void calcDeltas(gesture_data_type *gData) {
	gData->u_d_rise_delta = gData->u_upFront_pos - gData->d_upFront_pos;
	gData->u_d_fall_delta = gData->u_downFront_pos - gData->d_downFront_pos;

	if ((gData->u_d_rise_delta > 0) && (gData->u_d_fall_delta < 0)) {
		if(DEBUG){
			ESP_LOGD(TAG," vertical delta mistach  ");
		}
		gData->vertical_delta = 0;
	} else if ((gData->u_d_rise_delta < 0) && (gData->u_d_fall_delta > 0)) {
		if(DEBUG){
			ESP_LOGD(TAG," vertical delta mistach  ");
		}
		gData->vertical_delta = 0;
	} else {
		gData->vertical_delta = gData->u_d_rise_delta + gData->u_d_fall_delta;
	}

	gData->r_l_rise_delta = gData->r_upFront_pos - gData->l_upFront_pos;
	gData->r_l_fall_delta = gData->r_downFront_pos - gData->l_downFront_pos;

	if ((gData->r_l_rise_delta > 0) && (gData->r_l_fall_delta < 0)) {
		if(DEBUG){
			ESP_LOGD(TAG," horizontal delta mistach  ");
		}
		gData->horizontal_delta = 0;
	} else if ((gData->r_l_rise_delta < 0) && (gData->r_l_fall_delta > 0)) {
		if(DEBUG){
			ESP_LOGD(TAG," horizontal delta mistach  ");
		}
		gData->horizontal_delta = 0;
	} else {
		gData->horizontal_delta = gData->r_l_rise_delta + gData->r_l_fall_delta;
	}

	if(DEBUG){
		ESP_LOGD(TAG," Delta calculated vertical:%d horizontal:%d  ", gData->vertical_delta, gData->horizontal_delta);
	}
}

esp_err_t processGesture(gesture_data_type *gData) {
	// gesture_typeDef gest;
	// gest.dir = WAITING;
	// gest.speed = 0;

	if (gData->size > 4){
		searhchMaxMidVal(gData);
		searhchFrontIntersectPos(gData);
		calcDeltas(gData);
		// gest.speed = gData->size * 2;
		// if (gest.speed < 40) {
		// 	gest.speed = 40;
		// }
		if (abs(gData->vertical_delta) > abs(gData->horizontal_delta)) {
			//vertical swipe
			if (gData->vertical_delta > 0) {
				gData->gesture = SWIPE_DOWN;
			} else if (gData->vertical_delta < 0) {
				gData->gesture = SWIPE_UP;
			} else {
				gData->gesture = NOT_RECOGNIZED;
			}
		} else {
			if (gData->horizontal_delta > 0) {
				gData->gesture = SWIPE_LEFT;
			} else if (gData->horizontal_delta < 0) {
				gData->gesture = SWIPE_RIGHT;
			} else {
				gData->gesture = NOT_RECOGNIZED;
			}
		}
	}
	return ESP_OK;
}
