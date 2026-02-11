
file(GLOB_RECURSE MODULE_FILES "wavPlayer.c")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()

idf_component_register(SRCS 
                    "wav_handle.c"
					${MODULE_FILES} 
					${GENERATED_FILES}
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES me_slot_config reporter arsenal audio_sal audio_pipeline audio_stream audio_hal fatfs stateConfig my_board esp_peripherals )
