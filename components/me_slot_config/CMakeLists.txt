file(GLOB_RECURSE MODULE_FILES "adc1.c" "analog.c" "3n_mosfet.c" "encoders.c" "virtual_slots.c" ) 

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()


idf_component_register(SRCS
						"executor.c"
                        "stdreport.c"
                        "stdcommand.c"
						"flywheel.c"
                        "sensor_2ch.c"
                        
						"in_out.c"
						${MODULE_FILES}
                        
                       
                        "tenzo_button.c"
                        "me_slot_config.c"
                        "tachometer.c"

						${GENERATED_FILES}

                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES myCDC stepper stateConfig reporter audioPlayer audioLAN driver rgbHsv myHID someUnique distanceSens accel servoDev ticketDispenser dwin VESC PPM CRSF
)


# esp32-rotary-encoder
