idf_component_register(SRCS 
                        "flywheel.c" 
                        "tenzo_button.c" 
                        "sensor_2ch.c" 
                        "buttonLed.c" 
                        "smartLed.c" 
                        "in_out.c" 
                        "stepper.c" 
                        "analog.c" 
                        "virtual_slots.c"
                        "encoders.c" 
                        "me_slot_config.c" 
                        "executor.c" 
                        "3n_mosfet.c" 
                        "TOFs.c" 
                        "tachometer.c"
                       
                    INCLUDE_DIRS "include"
                    REQUIRES accelStepper stateConfig reporter audioPlayer esp32-rotary-encoder driver rgbHsv myHID someUnique distanceSens accel cybergear ticketDispenser dwin)

 