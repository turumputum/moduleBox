idf_component_register(SRCS "asyncStepper.c" "stepper.c"
                    INCLUDE_DIRS "include"
                    REQUIRES accelStepper stateConfig reporter)
