file(GLOB_RECURSE MODULE_FILES "analog.c" "tenzo_button.c")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()

idf_component_register(SRCS
                        ${MODULE_FILES}
                        ${GENERATED_FILES}
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES
                        stateConfig
                        reporter
                        me_slot_config
                        driver
                        esp_adc
                        esp_peripherals)
