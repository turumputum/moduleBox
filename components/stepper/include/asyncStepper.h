
#include <stdlib.h>
// #include "driver/mcpwm.h"
// #include "soc/mcpwm_struct.h" 
// #include "soc/mcpwm_reg.h"
#include "driver/gpio.h"
#include "driver/mcpwm_prelude.h"
#include "driver/pulse_cnt.h"

#define MCPWM_MIN_FREQUENCY 16
#define MCPWM_DUTY_US 2

#define PROCESS_MOVEMENT_PERIOD 10
#if PROCESS_MOVEMENT_PERIOD < 1
  #error PROCESS_MOVEMENT_PERIOD "PROCESS_MOVEMENT_PERIOD CAN'T BE LESS THAN 1"
#endif

typedef enum {
  STEPPER_MOTOR_STOPPED,
  STEPPER_MOTOR_ACCELERATING,
  STEPPER_MOTOR_AT_SPEED,
  STEPPER_MOTOR_DECELERATING,
  STEPPER_MOTOR_STOPPING
} asyncStepperStatus_t;

typedef enum {
  SPEED_STEPPER_STOP,
  SPEED_STEPPER_RUN
} speedStepperStatus_t;

typedef struct {
    gpio_num_t _stepPin;
    gpio_num_t _directionPin;
    //mcpwm_unit_t _mcpwmUnit;
    //mcpwm_io_signals_t _mcpwmIoSignalsPwm;
    mcpwm_timer_handle_t  _timer;
    mcpwm_oper_handle_t _oper;
    mcpwm_cmpr_handle_t _comparator;
    mcpwm_gen_handle_t _generator;
    
    pcnt_unit_handle_t _pcnt_unit;
    pcnt_channel_handle_t _pcnt_chan;
    uint32_t _resolution;
    uint8_t _state;
    bool _direction;
    bool _dirInverse;
    int32_t _currentSpeed;
    int32_t _minPeriod;
   
}speedStepper_t;

#define SPEED_STEPPER_DEFAULT() {\
	._stepPin = 0,\
	._directionPin = 0,\
	._timer = NULL,\
  ._oper = NULL,\
  ._comparator = NULL,\
  ._generator = NULL,\
	._direction = 0,\
  ._dirInverse = 0,\
  ._resolution = 1000000,\
  ._currentSpeed = 0,\
  ._minPeriod = 5,\
  ._state = SPEED_STEPPER_STOP,\
}



void speedStepper_init(speedStepper_t *stepper, gpio_num_t step_pin, gpio_num_t dir_pin, uint8_t pulseWidth);
// int32_t get_position(asyncStepper_t *stepper);
void speedStepper_stop(speedStepper_t *stepper);
void speedStepper_start(speedStepper_t *stepper);
void speedStepper_setSpeed(speedStepper_t *stepper, int32_t spd);
void speedStepper_setDirection(speedStepper_t *stepper, int8_t clockwise);
// void setSpeed(asyncStepper_t *stepper, int32_t spd);
// void startStepper(asyncStepper_t *stepper);

// class ESP32AsyncStepper
// {
//   public:
//     ESP32AsyncStepper(gpio_num_t stepPinNumber, gpio_num_t directionPinNumber, 
//       mcpwm_unit_t mcpwmUnit = MCPWM_UNIT_0, mcpwm_io_signals_t mcpwmIoSignals = MCPWM0A, mcpwm_timer_t mcpwmTimer = MCPWM_TIMER_0);
//     void begin(); 
//     void stopMotor();
//     asyncStepperStatus_t getMotorStatus();

//     //STEPS functions
//     void SetPositionInSteps(long currentPositionInSteps);
//     long getCurrentPositionInSteps();
//     void setSpeedInStepsPerSecond(float speedInStepsPerSecond);
//     void setAccelerationInStepsPerSecondPerSecond(float accelerationInStepsPerSecondPerSecond);
//     void relativeMoveInSteps(long distanceToMoveInSteps);
//     void absoluteMoveInSteps(long absolutePositionToMoveToInSteps);

//     //Continuous moving functions
//     void runContinuous(bool run, bool direction);

//     bool motionComplete();


//   private:
//     static void IRAM_ATTR isr_handler (void *arg);
//     static void taskProcessMovement (void *arg);
//     static void taskProcessContinuousMovement (void *arg);

//     gpio_num_t _stepPin;
//     gpio_num_t _directionPin;
//     mcpwm_unit_t _mcpwmUnit;
//     mcpwm_io_signals_t _mcpwmIoSignalsPwm;
//     mcpwm_timer_t _mcpwmTimer;
//     asyncStepperStatus_t _motorStatus;
//     bool _direction;
//     double _desiredSpeedInStepsPerSecond;
//     double _accelerationInStepsPerSecondPerSecond;
//     long _decelerationDistanceInSteps;
//     long _targetPositionInSteps;
//     uint32_t _frequency;
//     volatile long _currentPositionInSteps = 0;
// };