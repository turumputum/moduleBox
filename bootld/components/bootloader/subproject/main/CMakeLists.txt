idf_component_register(SRCS "bootloader_start.c"
                    INCLUDE_DIRS "../../../../boards/${BOARD}"
                    REQUIRES bootloader bootloader_support)

idf_build_get_property(target IDF_TARGET)
set(scripts "ld/${target}/bootloader.ld")

if(NOT CONFIG_IDF_TARGET_ESP32H2)
    list(APPEND scripts "ld/${target}/bootloader.rom.ld")
endif()

target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
