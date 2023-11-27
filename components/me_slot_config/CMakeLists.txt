idf_component_register(SRCS "buttonLed.c" "smartLed.c" "in_out.c" "stepper.c" "analog.c" "rfid.c" "encoders.c" "in_out.c" "me_slot_config.c" "executor.c" "3n_mosfet.c" "encoders.c" "rfid.c" "TOFs.c" "tachometer.c" "analog.c"
                    INCLUDE_DIRS "include"
                    REQUIRES accelStepper stateConfig reporter audioPlayer reporter esp32-rotary-encoder driver)

 