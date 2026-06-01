file(GLOB_RECURSE MODULE_FILES
    "stepper.c"
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()

idf_component_register(SRCS
                        "asyncStepper.c"
                        ${GENERATED_FILES}
                        ${MODULE_FILES}
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES accelStepper stateConfig reporter me_slot_config)
