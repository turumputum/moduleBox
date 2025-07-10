

file(GLOB_RECURSE MODULE_FILES "adc1.c" "analog.c" "buttonLed.c" "3n_mosfet.c" "encoders.c" "smartLed.c"  )

include(${CMAKE_SOURCE_DIR}/CMakeManifest.txt OPTIONAL)

idf_component_register(SRCS 
						"executor.c"
                        "stdreport.c"
                        "stdcommand.c"
						"flywheel.c"
                        "sensor_2ch.c" 

						"in_out.c"  
						${MODULE_FILES}

                        "tenzo_button.c" 
                        "virtual_slots.c"
                        "me_slot_config.c" 
                        "tachometer.c"

						${GENERATED_FILES}
                       
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES myCDC stepper stateConfig reporter audioPlayer esp32-rotary-encoder driver rgbHsv myHID someUnique distanceSens accel servoDev ticketDispenser dwin VESC PPM CRSF
)


