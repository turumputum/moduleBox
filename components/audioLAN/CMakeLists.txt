file(GLOB_RECURSE MODULE_FILES "audioLAN.c")

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()


idf_component_register(SRCS 
					${MODULE_FILES}
					${GENERATED_FILES} "rtp_client_stream.c"
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES audio_hal audio_stream audio_sal stateConfig reporter)
