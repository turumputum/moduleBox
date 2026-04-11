file(GLOB_RECURSE MODULE_FILES "adc1.c" "analog.c" "encoders.c" "testsd.c") 

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()


idf_component_register(SRCS
						"executor.c"
                        "stdreport.c"
                        "stdcommand.c"
						"flywheel.c"
                       
						${MODULE_FILES}

                        "tenzo_button.c"
                        "me_slot_config.c"
                        "tachometer.c"

						${GENERATED_FILES}

                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES 
                        in_out 
                        myCDC 
                        stepper 
                        stateConfig 
                        reporter 
                        audioPlayer 
                        audioLAN 
                        opusLAN
                        driver 
                        rgbHsv 
                        myHID 
                        someUnique 
                        distanceSens 
                        lidars
                        accel 
                        servoDev 
                        dwin 
                        PPM 
                        virtualSlots 
                        3n_MOSFET 
                        esp_peripherals
                        phoneDialer
)


# esp32-rotary-encoder
