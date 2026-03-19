file(GLOB_RECURSE MODULE_FILES "startup.c"
                            "counter.c"
                            "random.c"
                            "timer.c"
                            "flywheel.c"
                            
                            "whitelist.c"
                            "collector.c"
                            "scaler.c"
                            
                            "conductor.c"
                            "masquerade.c") 

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)
endif()

idf_component_register(SRCS "masquerade.c" "virtualSlots.c"
                        "tankControl.c"
                        "watchdog.c"
                        ${MODULE_FILES}  
                        ${GENERATED_FILES}
    
                        INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                        REQUIRES  stateConfig reporter me_slot_config
    )
