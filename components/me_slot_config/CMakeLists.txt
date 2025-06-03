idf_component_register(SRCS 
                        "flywheel.c" 
                        "tenzo_button.c" 
                        "sensor_2ch.c" 
                        "buttonLed.c" 
                        "smartLed.c" 
                        "in_out.c" 
                        
                        "analog.c" 
                        "adc1.c" 
                        "virtual_slots.c"
                        "encoders.c" 
                        "me_slot_config.c" 
                        "executor.c" 
                        "3n_mosfet.c" 
                        
                        "tachometer.c"
                       
                    INCLUDE_DIRS "include"
                    REQUIRES myCDC stepper stateConfig reporter audioPlayer driver rgbHsv myHID someUnique distanceSens accel servoDev ticketDispenser dwin VESC PPM CRSF)

 