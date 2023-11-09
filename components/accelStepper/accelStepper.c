 /**
 ******************************************************************************
 * @file    AccelStepper.c
 * @author  Matej Gomboc, Institute IRNAS Ra�e
 * @version V1.0.0
 * @date    01-July-2016
 * @brief   This is a modified version of AccelStepper Arduino library ported to
 *          STM32F4xx microcontrollers. Copyright Institute IRNAS Ra�e 2015 - info@irnas.eu.
 *          The original version of this library can be obtained at:
 *          <http://www.airspayce.com/mikem/arduino/AccelStepper/>. The original
 *          version is Copyright (C) 2009-2013 Mike McCauley.
 ******************************************************************************
 **/

#include <math.h>
#include "accelStepper.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "sdkconfig.h"

#include <esp_timer.h>
#include "driver/gptimer.h"


#include "driver/gpio.h"
#include "soc/gpio_struct.h"
// esp_timer_handle_t us_timer

//GPIO.out_w1ts
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
static const char *TAG = "STEPPER";


extern uint32_t testCount;
//esp_timer_handle_t us_timer;
gptimer_handle_t usTime;
gptimer_handle_t us_timer;


void moveTo(Stepper_t* motor, long absolute)
{
    if (motor->_targetPos != absolute)
    {
    	motor->_targetPos = absolute;
		computeNewSpeed(motor);
		// compute new n?
    }
}

void move(Stepper_t* motor, long relative)
{
    moveTo(motor, motor->_currentPos + relative);
}

// Implements steps according to the current step interval
// You must call this at least once per step
// returns true if a step occurred
uint8_t runSpeed(Stepper_t* motor)
{
    // Dont do anything unless we actually have a step interval
	testCount = motor->_stepInterval;
    if (!motor->_stepInterval) return 0; // false
	
	//unsigned long time = HAL_GetTick() * 10; //Arduino: micros();
    //unsigned long time = esp_timer_get_time();
	unsigned long time;
	gptimer_get_raw_count(usTime, &time);
	unsigned long nextStepTime = motor->_lastStepTime + motor->_stepInterval;

	// Gymnastics to detect wrapping of either the nextStepTime and/or the current time
	if (((nextStepTime >= motor->_lastStepTime) && ((time >= nextStepTime) || (time < motor->_lastStepTime)))
	|| ((nextStepTime < motor->_lastStepTime) && ((time >= nextStepTime) && (time < motor->_lastStepTime))))
	{
		if (motor->_direction == DIRECTION_CW)
		{
			// Clockwise
			motor->_currentPos += 1;
		}
		else
		{
			// Anticlockwise
			motor->_currentPos -= 1;
		}

		step(motor, motor->_currentPos);

		motor->_lastStepTime = time;

		return 1; // true
    }
    else
    {
    	return 0; // false
    }
}

long distanceToGo(Stepper_t* motor)
{
    return (motor->_targetPos - motor->_currentPos);
}

long targetPosition(Stepper_t* motor)
{
    return motor->_targetPos;
}

long currentPosition(Stepper_t* motor)
{
    return motor->_currentPos;
}

// Useful during initialisations or after initial positioning
// Sets speed to 0
void setCurrentPosition(Stepper_t* motor, long position)
{
	motor->_targetPos = motor->_currentPos = position;
	motor->_n = 0;
	motor->_stepInterval = 0;
	motor->_speed = 0;
}

void computeNewSpeed(Stepper_t* motor)
{
    long distanceTo = distanceToGo(motor); // +ve is clockwise from curent location

    long stepsToStop = (long)((motor->_speed * motor->_speed) / (2 * motor->_acceleration)); // Equation 16
	//motor->_break_way = (motor->_speed * motor->_speed)/(2 * motor->_acceleration);
	motor->_break_way = stepsToStop;

    if (distanceTo == 0 && stepsToStop <= 1)
    {
    	// We are at the target and its time to stop
    	motor->_stepInterval = 0;
    	motor->_speed = 0;
    	motor->_n = 0;
    	return;
    }

    if (distanceTo > 0)
    {
		// We are anticlockwise from the target
		// Need to go clockwise from here, maybe decelerate now
		if (motor->_n > 0)
		{
			// Currently accelerating, need to decel now? Or maybe going the wrong way?
			if ((stepsToStop >= distanceTo) || motor->_direction == DIRECTION_CCW)
				motor->_n = -stepsToStop; // Start deceleration
		}
		else if (motor->_n < 0)
		{
			// Currently decelerating, need to accel again?
			if ((stepsToStop < distanceTo) && motor->_direction == DIRECTION_CW)
				motor->_n = -motor->_n; // Start accceleration
		}
	}
	else if (distanceTo < 0)
	{
		// We are clockwise from the target
		// Need to go anticlockwise from here, maybe decelerate
		if (motor->_n > 0){
			// Currently accelerating, need to decel now? Or maybe going the wrong way?
			if ((stepsToStop >= -distanceTo) || motor->_direction == DIRECTION_CW)
				motor->_n = -stepsToStop; // Start deceleration
		}
		else if (motor->_n < 0)
		{
			// Currently decelerating, need to accel again?
			if ((stepsToStop < -distanceTo) && motor->_direction == DIRECTION_CCW)
				motor->_n = -motor->_n; // Start accceleration
		}
	}

	// Need to accelerate or decelerate
	if (motor->_n == 0)
	{
		// First step from stopped
		motor->_cn = motor->_c0;
		motor->_direction = (distanceTo > 0) ? DIRECTION_CW : DIRECTION_CCW;
	}
	else
	{
		// Subsequent step. Works for accel (n is +_ve) and decel (n is -ve).
		motor->_cn = motor->_cn - ((2 * motor->_cn) / ((4 * motor->_n) + 1)); // Equation 13
		motor->_cn = (motor->_cn > motor->_cmin) ? motor->_cn : motor->_cmin; //max(motor->_cn, motor->_cmin);
	}

	motor->_n++;
	motor->_stepInterval = motor->_cn;
	motor->_speed = 1000000/ motor->_cn;

	if (motor->_direction == DIRECTION_CCW)
		motor->_speed = -motor->_speed;


#ifdef DEBUG_MODE
    printf("%f\n", motor->_speed);
    printf("%f\n", motor->_acceleration);
    printf("%f\n", motor->_cn);
    printf("%f\n", motor->_c0);
    printf("%ld\n", motor->_n);
    printf("%lu\n", motor->_stepInterval);
//    Serial.println(distanceTo);
//    Serial.println(stepsToStop);
#endif
}


void check_sensors_queue(Stepper_t* motor){
	int8_t val;
	if(motor->_sensor_interupt_flag==1){
		motor->_sensor_interupt_flag=0;
		if(gpio_get_level(motor->_pin_sensor_up)==(!motor->_inv_sensor_up)){
			moveTo(motor, currentPosition(motor)+motor->_break_way);
			motor->_up_sensor_found=1;
			sprintf(motor->report_msg,"Stop on UP sensor, curPos:%ld, tarPos:%ld", currentPosition(motor), targetPosition(motor));
		}
		if(gpio_get_level(motor->_pin_sensor_down)==(!motor->_inv_sensor_down)){
			moveTo(motor, currentPosition(motor)-motor->_break_way);
			motor->_down_sensor_found=1;
			sprintf(motor->report_msg,"Stop on DOWN sensor, curPos:%ld, tarPos:%ld", currentPosition(motor), targetPosition(motor));
		}
	}
}
// Run the motor to implement speed and acceleration in order to proceed to the target position
// You must call this at least once per step, preferably in your main loop
// If the motor is in the desired position, the cost is very small
// returns true if the motor is still running to the target position.
uint8_t run(Stepper_t* motor)
{

	if(motor->_stop_on_sensor){
		if((motor->_pin_sensor_up>=0)||(motor->_pin_sensor_down>=0)){
			check_sensors_queue(motor);
		}
	}

    if (runSpeed(motor)) computeNewSpeed(motor);

    return motor->_speed != 0 || distanceToGo(motor) != 0;
	//return distanceToGo(motor) != 0;
	//return 1;
}


//static bool IRAM_ATTR step_callback(us_timer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg){
static bool IRAM_ATTR timer_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg){
		
	Stepper_t* motor = (Stepper_t*) arg;
	
	if(motor->_pulse_up_flag){
		GPIO.out_w1tc = ((uint32_t)1 << motor->_pin[0]);
		motor->_pulse_up_flag = 0;
	}else{
		if(motor->_stop_on_sensor){
			check_sensors_queue(motor);
		}
		run(motor);
	}
	return pdTRUE;
}

void InitStepper(Stepper_t* motor, uint8_t interface, uint16_t pin1, uint16_t pin2, uint8_t enable)
{
	motor->_interface = interface;
	motor->_currentPos = 0;
	motor->_targetPos = 0;
	motor->_speed = 0;
	motor->_maxSpeed = 1;
	motor->_acceleration = 0;
	motor->_sqrt_twoa = 1;
	motor->_stepInterval = 0;
	motor->_minPulseWidth = 2;
	motor->_enablePin = 0xff;
	motor->_lastStepTime = 0;
	motor->_pin[0] = pin1;
	motor->_pin[1] = pin2;

	motor->_stop_on_sensor=0;
	motor->_break_way=0;

	motor->_sensor_interupt_flag=0;

	motor->_pin_sensor_up = -1;
	motor->_pin_sensor_down = -1;

	motor->_inv_sensor_up=0;
	motor->_inv_sensor_down=0;

	motor->_up_sensor_found=0;
	motor->_down_sensor_found=0;
	
    // NEW
	motor->_n = 0;
	motor->_c0 = 0.0;
    motor->_cn = 0.0;
    motor->_cmin = 1.0;
    motor->_direction = DIRECTION_CCW;

	motor->init_state=-1;

    int i;
    for (i = 0; i < 2; i++)
    	motor->_pinInverted[i] = 0;

    enableOutputs(motor);

    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, // 1MHz, 1 tick=1us
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &us_timer));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = timer_callback,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(us_timer, &cbs, motor));
    ESP_ERROR_CHECK(gptimer_enable(us_timer));

    gptimer_alarm_config_t run_timer_config = {
        .reload_count = 0,
        .alarm_count = 10, // period = 10us
        .flags.auto_reload_on_alarm = true,
    };
    
    ESP_ERROR_CHECK(gptimer_set_alarm_action(us_timer, &run_timer_config));
    ESP_ERROR_CHECK(gptimer_start(us_timer));

	// us time
	gptimer_config_t time_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000, // 1MHz, 1 tick=1us
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&time_config, &usTime));
	ESP_ERROR_CHECK(gptimer_enable(usTime));
	ESP_ERROR_CHECK(gptimer_start(usTime));
	
    // Some reasonable default
    setAcceleration(motor, 1);
}

static void IRAM_ATTR sensor_int_handler(void* arg)
{
    Stepper_t* motor = (Stepper_t*) arg;
    motor->_sensor_interupt_flag=1;
}

void set_stepper_sensor(Stepper_t* motor,int8_t pin_down, uint8_t inverse_down, int8_t pin_up, uint8_t inverse_up){
	
	gpio_config_t up_conf = {};
	if(inverse_up){
		motor->_inv_sensor_up=1;
    	up_conf.intr_type = GPIO_INTR_NEGEDGE;
	}else{
		motor->_inv_sensor_up=0;
		up_conf.intr_type = GPIO_INTR_POSEDGE;
	}
    //bit mask of the pins, use GPIO4/5 here
    up_conf.pin_bit_mask = (1ULL<<pin_up);
    //set as input mode
    up_conf.mode = GPIO_MODE_INPUT;
	motor->_pin_sensor_up = pin_up;
    gpio_config(&up_conf);
	gpio_set_intr_type(pin_up, GPIO_INTR_ANYEDGE);

	gpio_config_t down_conf = {};
	if(inverse_down){
		motor->_inv_sensor_down=1;
    	down_conf.intr_type = GPIO_INTR_NEGEDGE;
	}else{
		motor->_inv_sensor_down=0;
		down_conf.intr_type = GPIO_INTR_POSEDGE;
	}
    //bit mask of the pins, use GPIO4/5 here
    down_conf.pin_bit_mask = (1ULL<<pin_down);
    //set as input mode
    down_conf.mode = GPIO_MODE_INPUT;
	motor->_pin_sensor_down = pin_down;
    gpio_config(&down_conf);
	gpio_install_isr_service(0);

	gpio_isr_handler_add(pin_up, sensor_int_handler, (void *)motor);
	gpio_isr_handler_add(pin_down, sensor_int_handler, (void *)motor);
}


void setMaxSpeed(Stepper_t* motor, int32_t speed)
{
    if (motor->_maxSpeed != speed)
    {
    	motor->_maxSpeed = speed;
    	motor->_cmin = 1000000.0 / speed;
		// Recompute _n from current speed and adjust speed if accelerating or cruising
		if (motor->_n > 0)
		{
			motor->_n = (long)((motor->_speed * motor->_speed) / (2 * motor->_acceleration)); // Equation 16
			computeNewSpeed(motor);
		}
    }
}

int32_t maxSpeed(Stepper_t* motor)
{
    return motor->_maxSpeed;
}

void setAcceleration(Stepper_t* motor, int32_t acceleration)
{
    if (acceleration == 0)
	return;
    if (motor->_acceleration != acceleration)
    {
	    // Recompute _n per Equation 17
    	motor->_n = motor->_n * (motor->_acceleration / acceleration);
		// New c0 per Equation 7, with correction per Equation 15
    	motor->_c0 = 0.676 * sqrt(2.0 / acceleration) * 1000000.0;// Equation 15
    	motor->_acceleration = acceleration;
		computeNewSpeed(motor);
    }
}

float constrain(float value, float minimum, float maximum)
{
	if(value < minimum) return minimum;
	else if(value > maximum) return maximum;
	else return value;
}

void setSpeed(Stepper_t* motor, int32_t speed)
{
    if (speed == motor->_speed)
        return;

    speed = constrain(speed, -motor->_maxSpeed, motor->_maxSpeed);

    if (speed == 0.0)
    {
    	motor->_stepInterval = 0;
    }
    else
    {
    	motor->_stepInterval = fabs(1000000.0 / speed);
    	motor->_direction = (speed > 0.0) ? DIRECTION_CW : DIRECTION_CCW;
    }

    motor->_speed = speed;
}

int32_t speed(Stepper_t* motor)
{
    return motor->_speed;
}

// You might want to override this to implement eg serial output
// bit 0 of the mask corresponds to _pin[0]
// bit 1 of the mask corresponds to _pin[1]
// ....
void setOutputPins(Stepper_t* motor, uint8_t mask)
{
    uint8_t numpins = 2;
	uint8_t i;
	for (i = 0; i < numpins; i++)
	{
		//Arduino: digitalWrite(motor->_pin[i], (mask & (1 << i)) ? (HIGH ^ motor->_pinInverted[i]) : (LOW ^ motor->_pinInverted[i]));
		if(mask & (1 << i))
		{
			if (0x11 ^ motor->_pinInverted[i]) gpio_set_level(motor->_pin[i], 1);
			else gpio_set_level(motor->_pin[i], 0);
		}
		else
		{
			if (0x00 ^ motor->_pinInverted[i]) gpio_set_level(motor->_pin[i], 1);
			else gpio_set_level(motor->_pin[i], 0);
		}
	}
}

// def struct Stepper_t* Stepper_handle_t; 



void step(Stepper_t* motor, long step)
{
	if(((motor->_direction)&&(!motor->_dirInverted))||((!motor->_direction)&&(motor->_dirInverted))){
		GPIO.out_w1ts = ((uint32_t)1 << motor->_pin[1]);
	}else if(((motor->_direction)&&(motor->_dirInverted))||((!motor->_direction)&&(!motor->_dirInverted))){
		GPIO.out_w1tc = ((uint32_t)1 << motor->_pin[1]);
	}

	GPIO.out_w1ts = ((uint32_t)1 << motor->_pin[0]);
	motor->_pulse_up_flag=1;

}
   

// Prevents power consumption on the outputs
void disableOutputs(Stepper_t* motor)
{
    if (! motor->_interface) return;

    setOutputPins(motor, 0); // Handles inversion automatically

    if (motor->_enablePin != 0xff) // If enable pin used
    {
        //Arduino: pinMode(motor->_enablePin, OUTPUT);
    	gpio_config_t io_conf ={
			.intr_type = GPIO_INTR_DISABLE,
			.mode = GPIO_MODE_OUTPUT,
			.pin_bit_mask = (1ULL << motor->_pin[0]),
			.pull_down_en = 0,
			.pull_up_en = 1
		};

        //Arduino: digitalWrite(motor->_enablePin, LOW ^ motor->_enableInverted);
        if (0x00 ^ motor->_enableInverted) gpio_set_level(motor->_enablePin, 1);
        else gpio_set_level(motor->_enablePin, 0);
    }
}

void enableOutputs(Stepper_t* motor)
{
    if (! motor->_interface) return;

	gpio_config_t io_conf_p0 ={
			.intr_type = GPIO_INTR_DISABLE,
			.mode = GPIO_MODE_OUTPUT,
			.pin_bit_mask = (1ULL << motor->_pin[0]),
			.pull_down_en = 0,
			.pull_up_en = 1
		};

	gpio_config(&io_conf_p0);

	gpio_config_t io_conf_p1 ={
			.intr_type = GPIO_INTR_DISABLE,
			.mode = GPIO_MODE_OUTPUT,
			.pin_bit_mask = (1ULL << motor->_pin[1]),
			.pull_down_en = 0,
			.pull_up_en = 1
		};

	gpio_config(&io_conf_p1);
	
    if (motor->_enablePin != 0xff)
    {
    	gpio_config_t io_conf_e ={
			.intr_type = GPIO_INTR_DISABLE,
			.mode = GPIO_MODE_OUTPUT,
			.pin_bit_mask = (1ULL << motor->_enablePin),
			.pull_down_en = 0,
			.pull_up_en = 1
		};

		gpio_config(&io_conf_e);

		if (0x11 ^ motor->_enableInverted) gpio_set_level(motor->_enablePin, 1);
		else gpio_set_level(motor->_enablePin, 0);
    }
}

void setMinPulseWidth(Stepper_t* motor, unsigned int minWidth)
{
	motor->_minPulseWidth = minWidth;
}

void setEnablePin(Stepper_t* motor, uint16_t enablePin)
{
	motor->_enablePin = enablePin;

    // This happens after construction, so init pin now.
    if (motor->_enablePin != 0xff)
    {
        //Arduino: pinMode(motor->_enablePin, OUTPUT);
		gpio_config_t io_conf ={
			.intr_type = GPIO_INTR_DISABLE,
			.mode = GPIO_MODE_OUTPUT,
			.pin_bit_mask = (1ULL << motor->_enablePin),
			.pull_down_en = 0,
			.pull_up_en = 1
		};

		gpio_config(&io_conf);
    	//Arduino: digitalWrite(motor->_enablePin, HIGH ^ motor->_enableInverted);
		if (0x11 ^ motor->_enableInverted) gpio_set_level(motor->_enablePin, 1);
		else gpio_set_level(motor->_enablePin, 0);
    }
}

void setPinsInvertedStpDir(Stepper_t* motor, uint8_t directionInvert, uint8_t stepInvert, uint8_t enableInvert)
{
	motor->_pinInverted[0] = stepInvert; // bool
	motor->_pinInverted[1] = directionInvert; // bool
	motor->_enableInverted = enableInvert; // bool
}

void setPinsInverted(Stepper_t* motor, uint8_t pin1Invert, uint8_t pin2Invert, uint8_t pin3Invert, uint8_t pin4Invert, uint8_t enableInvert)
{
	motor->_pinInverted[0] = pin1Invert; // bool
	motor->_pinInverted[1] = pin2Invert; // bool
	motor->_pinInverted[2] = pin3Invert; // bool
	motor->_pinInverted[3] = pin4Invert; // bool
	motor->_enableInverted = enableInvert; // bool
}

// Blocks until the target position is reached and stopped
void runToPosition(Stepper_t* motor)
{
    while (run(motor)) ;
}

uint8_t runSpeedToPosition(Stepper_t* motor)
{
    if (motor->_targetPos == motor->_currentPos)
    	return 0; // false
    if (motor->_targetPos > motor->_currentPos)
    	motor->_direction = DIRECTION_CW;
    else
    	motor->_direction = DIRECTION_CCW;

    return runSpeed(motor);
}

// Blocks until the new target position is reached
void runToNewPosition(Stepper_t* motor, long position)
{
    moveTo(motor, position);
    runToPosition(motor);
}

void stop(Stepper_t* motor)
{
    if (motor->_speed != 0)
    {
		long stepsToStop = (long)((motor->_speed * motor->_speed) / (2 * motor->_acceleration)) + 1; // Equation 16 (+integer rounding)

		if (motor->_speed > 0)
			move(motor, stepsToStop);

		else
			move(motor, -stepsToStop);
    }
}

uint8_t isRunning(Stepper_t* motor)
{
    return !(motor->_speed == 0 && motor->_targetPos == motor->_currentPos);
}


