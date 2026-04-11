file(GLOB_RECURSE MODULE_FILES 
    "lidars_rplidarS1.c"
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()

idf_component_register(SRCS
                        "lidars_common.c"
                        ${GENERATED_FILES}
                        ${MODULE_FILES}
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES stateConfig)
