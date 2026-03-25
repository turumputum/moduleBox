if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${CMAKE_HOME_DIRECTORY}/CMakeManifest.txt)

    set(GENERATED_MODULES_H "${CMAKE_CURRENT_BINARY_DIR}/manifest_modules.h")

    add_custom_command(
        OUTPUT "${GENERATED_MODULES_H}"
        COMMAND ${CMAKE_COMMAND}
            -DCACHE_FILE=${CMAKE_BINARY_DIR}/CMakeCache.txt
            -DOUTPUT_FILE=${GENERATED_MODULES_H}
            -P ${CMAKE_CURRENT_LIST_DIR}/gen_manifest_modules.cmake
        DEPENDS ${CMAKE_BINARY_DIR}/CMakeCache.txt
        COMMENT "Generating manifest_modules.h from CMakeCache"
    )

    add_custom_target(gen_manifest_modules_h DEPENDS "${GENERATED_MODULES_H}")
endif()


idf_component_register(SRCS "manifest.c"
                    INCLUDE_DIRS "include" ${CMAKE_CURRENT_BINARY_DIR}
                    REQUIRES stateConfig arsenal )

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_dependencies(__idf_manifest gen_manifest_modules_h)
endif()


