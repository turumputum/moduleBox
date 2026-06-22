file(GLOB_RECURSE MODULE_FILES "testsd.c")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()

idf_component_register(SRCS
                        "someUnique.c"
                        ${MODULE_FILES}
                        ${GENERATED_FILES}
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES mqtt stateConfig st7789 me_slot_config reporter driver esp_peripherals)
