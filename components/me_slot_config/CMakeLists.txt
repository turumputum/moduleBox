idf_component_register(SRCS
						"executor.c"
                        "stdreport.c"
                        "stdcommand.c"
                        "me_slot_config.c"

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
                        buttonMatrix
                        distanceSens
                        encoders
                        lidars
                        accel 
                        servoDev 
                        dwin 
                        PPM 
                        virtualSlots 
                        3n_MOSFET 
                        esp_peripherals
                        phoneDialer
                        adc
)


# esp32-rotary-encoder
