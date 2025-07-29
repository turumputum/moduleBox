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


file(GLOB_RECURSE MODULE_FILES "adc1.c" "analog.c" "buttonLed.c" "3n_mosfet.c" "encoders.c" "smartLed.c"  )


#if(NOT CMAKE_BUILD_EARLY_EXPANSION)
#include(${CMAKE_SOURCE_DIR}/../CMakeManifest.txt)
#endif()


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


