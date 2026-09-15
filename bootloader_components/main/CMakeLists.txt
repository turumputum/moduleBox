# Переопределение компонента main второго загрузчика ESP-IDF (bootloader_components/main -
# штатный механизм IDF, см. docs/en/api-guides/build-system.rst "Custom bootloader").
# Повторяет $IDF_PATH/components/bootloader/subproject/main/CMakeLists.txt, отличается
# только bootloader_start.c - выбор раздела bootldsd/program по причине сброса и RTC-подсказке.
idf_component_register(SRCS "bootloader_start.c"
                    INCLUDE_DIRS "."
                    REQUIRES bootloader bootloader_support)

idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
