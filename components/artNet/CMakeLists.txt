file(GLOB_RECURSE MODULE_FILES "artNetCrosslink.c")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()

idf_component_register(SRCS
                        "artNet_common.c"
                        ${MODULE_FILES}
                        ${GENERATED_FILES}
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES
                        stateConfig
                        reporter
                        me_slot_config
                        esp_netif
                        lwip
                        esp_timer)
