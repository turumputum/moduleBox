# bootldsd (loader) - загрузчик обновлений moduleBox (раздел bootldsd, ota_0 @ 0x10000).
# Читает /int/UPDATE.FW из раздела storage (FAT поверх wear levelling) и пишет в раздел
# program (factory @ 0x50000). Карта памяти не используется, пины платы не важны.
#
# Сборка (тот же IDF v5.5.4, что и у основного проекта):
#   idf.py build            -> build/bootldsd.bin
# Таблица разделов берётся из корня репозитория (../../partitions.csv) - единый источник.
#
# idf.py flash отсюда шьёт ТОЛЬКО build/bootldsd.bin в 0x10000 (см. ниже). Bootloader и
# таблицу разделов собирает и шьёт основной проект (idf.py flash в корне репозитория), он же
# льёт bootldsd/bootldsd.bin - после сборки скопируй build/bootldsd.bin в bootldsd/.
# Полный комплект одной командой: bootldsd/flash_kit.cmd.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bootldsd)

# Штатные цели flash/app-flash целятся в boot-default раздел (factory = program @ 0x50000)
# и льют туда загрузчик, затирая прошивку. Оставляем в них только bootldsd @ 0x10000.
idf_build_get_property(build_dir BUILD_DIR)
foreach(tgt flash app-flash)
    foreach(prop FLASH_FILE FLASH_ENTRY IMAGES)
        get_property(lst TARGET ${tgt} PROPERTY ${prop})
        set(lst_filtered "")
        foreach(item IN LISTS lst)
            if(NOT item MATCHES "0x50000" AND NOT item MATCHES "bootloader/bootloader.bin")
                list(APPEND lst_filtered "${item}")
            endif()
        endforeach()
        set_property(TARGET ${tgt} PROPERTY ${prop} "${lst_filtered}")
    endforeach()
    esptool_py_flash_to_partition(${tgt} "bootldsd" "${build_dir}/bootldsd.bin")
endforeach()
