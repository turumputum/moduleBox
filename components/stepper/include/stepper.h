#include <stdio.h>
#include <driver/gptimer.h>
#include <driver/rmt_tx.h>

// #include <inttypes.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/queue.h"

// typedef struct{
// 	/// Pin number assignments for the 2 or 4 pins required to interface to the
// 	/// stepper motor or driver.
// 	uint16_t _pin[2];

// 	/// Whether the _pins is inverted or not.
// 	uint8_t _pinInverted[2]; // bool

// 	/// The current absolution position in steps.
// 	int64_t _currentPos; // Steps

// 	/// The current motos speed in steps per second.
// 	/// Positive is clockwise.
// 	int32_t _targetSpeed; // Steps per second
// 	//target speed in microsteps
// 	int32_t _targetSpeedMicro; 

// 	/// The current speed in steps per second.
// 	int32_t _currentSpeed; // Steps per second
	
// 	//current speed in microsteps
// 	int32_t _currentSpeedMicro; 

// 	/// The acceleration to use to accelerate or decelerate the motor in  Must be > 0.
// 	int32_t _acceleration;	/// steps per second per second.

	
	
// 	//precalculated interval increment per 10 nanoseconds
// 	//int32_t _stepIntervalIteration;
// 	//int32_t _sqrt_twoa; // Precomputed sqrt(2*_acceleration)

// 	/// The current interval between steps in microseconds.
// 	/// 0 means the motor is currently stopped with _speed == 0.
// 	int32_t _stepInterval;

//     int32_t _MAXstepInterval;

//     int32_t _MINstepInterval;

// 	/// The last step time in microseconds.
// 	int64_t _lastStepTime;

// 	/// The minimum allowed pulse width in nanoseconds.
// 	int64_t _minPulseWidth;

// 	/// Is the direction pin inverted?
// 	uint8_t _dirInverted; // bool /// Moved to _pinInverted[1]

// 	/// Is the step pin inverted?
// 	///uint8_t _stepInverted; // bool /// Moved to _pinInverted[0]

// 	/// Is the enable pin inverted?
// 	//uint8_t _enableInverted; // bool

// 	/// Enable pin for stepper driver, or 0xFFFF if unused.
// 	//uint16_t _enablePin;

// 	/// Current direction motor is spinning in.
// 	uint8_t _direction; // 1 == CW // possible values 0 or 1

// 	int32_t _break_way;
// 	//minimal steps to stop

// 	int32_t _preCount;

// 	uint8_t _pulse_up_flag;

// 	gptimer_handle_t _timer;

// } stepperSpeed_t;



typedef struct {
	int32_t currentSpeed;//steps per second
	int32_t targetSpeed;//steps per second
	int32_t acceleration;//steps per second per second
	int32_t minSpeed;//steps per second

	uint8_t dir;
	uint8_t dirInverse;

	uint8_t state;
	uint8_t prevState;

	uint8_t pulseInverse;

	uint32_t numOfSamples;//1 samples per second

	uint32_t resolution_hz;

	rmt_channel_handle_t rmt_channel;

	rmt_transmit_config_t rmt_tx_config;

	rmt_encoder_handle_t accel_encoder;
	rmt_encoder_handle_t decel_encoder;
	rmt_encoder_handle_t uniform_encoder;

	uint8_t intFlag;

} stepperSpeed_t;

#define RUN_STATE 1
#define STOP_STATE 0

#define STEPPER_SPEED_DEFAULT() {\
	.currentSpeed = 0,\
	.targetSpeed = 0,\
	.acceleration = 100,\
	.minSpeed = 100,\
	.dir = 0,\
	.dirInverse = 0,\
	.pulseInverse = 0,\
	.numOfSamples = 0,\
	.resolution_hz = 1000000,\
	.state = 0,\
	.prevState = 0,\
	.intFlag = 0,\
}


void start_stepperSpeed_task(int slot_num);

void testStepper();



void start_stepper_task(int num_of_slot);


void stepper_set_targetPos(char* str);
void stepper_set_currentPos(int32_t val);
void stepper_set_speed(int32_t speed);

void stepper_stop_on_sensor(int8_t val);



void start_sCurveStepper_task(int slot_num);



// typedef struct {
// 	int32_t currentSpeed;//steps per second
// 	int32_t targetSpeed;//steps per second
// 	int32_t maxSpeed;//steps per second
// 	int32_t targetPos;//steps
// 	int32_t currentPos;//steps
	
// 	int32_t accel;//steps per second per second

// } stepper_t;