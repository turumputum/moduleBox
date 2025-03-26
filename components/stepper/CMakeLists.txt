idf_component_register(SRCS "sCurveStepper.c" "asyncStepper.c" "stepper_motor_encoder.c" "stepper.c"
                    INCLUDE_DIRS "include"
                    REQUIRES accelStepper stateConfig reporter)
