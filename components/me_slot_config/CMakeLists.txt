

file(GLOB_RECURSE MODULE_FILES "adc1.c" "analog.c" "buttonLed.c" "3n_mosfet.c" "encoders.c"  )

foreach(SOURCE_FILE ${MODULE_FILES})
# Define the output file for the custom command (e.g., a generated header)
        get_filename_component(FILE_NAME "${SOURCE_FILE}" NAME_WE)
        set(GENERATED_OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated_files/${FILE_NAME}.h")

#message(GENERATED_OUTPUT_FILE="${GENERATED_OUTPUT_FILE}")

        add_custom_command(
            OUTPUT "${GENERATED_OUTPUT_FILE}"
            COMMAND manifesto ARGS "${SOURCE_FILE}" "${GENERATED_OUTPUT_FILE}"
            DEPENDS "${SOURCE_FILE}"
            COMMENT "Generating manifest of ${SOURCE_FILE}"
        )
        # Add the generated file to a list for later use in target
        list(APPEND GENERATED_FILES "${GENERATED_OUTPUT_FILE}")
endforeach()

#message(GENERATED_FILES="${GENERATED_FILES}")


idf_component_register(SRCS 
						"executor.c"
                        "stdreport.c"
                        "stdcommand.c"
						"smartLed.c"
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

