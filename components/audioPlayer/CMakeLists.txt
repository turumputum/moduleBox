
file(GLOB_RECURSE MODULE_FILES "audioPlayer.c"  )

#if(NOT DEFINED CMAKE_BUILD_EARLY_EXPANSION)
#include(${CMAKE_SOURCE_DIR}/../CMakeManifest.txt) 
#endif()

idf_component_register(SRCS 
					${MODULE_FILES}
					${GENERATED_FILES}
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES audio_sal audio_pipeline audio_stream audio_hal fatfs stateConfig my_board esp_peripherals )
