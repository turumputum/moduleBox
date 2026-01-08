
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)

add_compile_options(-DMODULE_FUNCTIONS=${MODULE_FUNCTIONS})
add_compile_options(-DMODULE_FUNCTIONS_DEFS=${MODULE_FUNCTIONS_DEFS})
endif()


idf_component_register(SRCS "manifest.c"
                    INCLUDE_DIRS "include"
                    REQUIRES stateConfig arsenal)


