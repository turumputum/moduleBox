file(GLOB_RECURSE MODULE_FILES "button_led.c" "button_ledBar.c" "button_ledRing.c" "button_smartLed.c" "button_swiperLed.c")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()

idf_component_register(SRCS
                        "button_logic.c"
                        "led_types.c"
                        ${GENERATED_FILES}
                        ${MODULE_FILES}
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES "me_slot_config" "stateConfig" "reporter" "rgbHsv" "arsenal"
)
