file(GLOB_RECURSE MODULE_FILES 
    "in_out.c"
    "outputs.c"
    "inputs.c"
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()

idf_component_register(SRCS
                        ${GENERATED_FILES}
                        ${MODULE_FILES}
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES "me_slot_config" "reporter" "stateConfig")

