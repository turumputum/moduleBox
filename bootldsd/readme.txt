bootldsd - механизм обновления прошивки moduleBox
==================================================

Имя историческое ("bootloader SD"): первая версия (bootldsd/bootldsd, 2022, монофон) искала
UPDATE.FW на карте памяти по SD-SPI на пинах 6/7/15/16. На платах moduleBox v3/v4 карта
сидит на SDMMC (47/21/40 или 41/40/3), поэтому тот загрузчик карту не видел и обновление
не применялось. С марта 2026 прошивка переносит UPDATE.FW во внутренний раздел flash,
и текущий загрузчик (bootldsd/loader) читает его именно оттуда. Карта ему не нужна.

Раскладка flash (partitions.csv в корне репозитория - единственный источник):
  0x00000  bootloader        кастомный второй загрузчик (bootloader_components/main в основном проекте)
  0x08000  partition table
  0x10000  bootldsd  ota_0   256K  загрузчик обновлений (этот проект, bootldsd/loader)
  0x50000  program   factory 3M    прошивка moduleBox
  0x350000 cfgbak            64K   резервная копия config.ini
  0x450000 storage   fat     3M    FAT+WL: /int для UPDATE.FW, а без карты - ещё и /sdcard

Как работает обновление:
  1. Файл UPDATE.FW (moduleBox.bin + заголовок от bin2fw.exe) заливается по FTP на карту
     под именем UPDATE.FW (переименование в UPDATE.FW - триггер).
  2. Прошивка (main/main.c moveUpdateToTheInternalStorage) переносит файл в /int (раздел
     storage), удаляет его с карты, ставит RTC-подсказку загрузчику и перезагружается.
  3. Bootloader: подсказка или аппаратный сброс (питание, EN, brownout) -> стартует bootldsd;
     программный сброс -> стартует program.
  4. bootldsd проверяет заголовок и CRC32, стирает program, пишет образ, перечитывает и
     сверяет, удаляет UPDATE.FW, esp_restart() -> program.
  Битый файл удаляется. При ошибке записи файл остаётся - следующий аппаратный сброс
  повторит, а негодный program bootloader сам подменит на bootldsd.

Как собрать:
  Загрузчик:   cd bootldsd/loader && idf.py build   -> build/bootldsd.bin, скопировать сюда.
  Bootloader и таблица: собираются основным проектом (idf.py build в корне):
               build/bootloader/bootloader.bin, build/partition_table/partition-table.bin.
  UPDATE.FW:   bin2fw.exe moduleBox.bin (делает deploy.bat).

Как прошить устройство полностью:
  flash_kit.cmd COM9   (Linux: ./flash_kit.sh /dev/ttyUSB0)   - все четыре бинарника из этой папки,
  или idf.py flash из корня репозитория - он шьёт то же самое (bootldsd.bin берёт из этой папки).
  Только прошивку: idf.py app-flash (0x50000) или UPDATE.FW по FTP.
  Из проекта bootldsd/loader idf.py flash шьёт только bootldsd.bin в 0x10000.

Единый бинарь для производства:
  deploy.bat собирает moduleBox_full.bin (esptool merge_bin: bootloader + таблица + bootldsd +
  прошивка, 8MB DIO 80m). Шьётся одним файлом в 0x0:
    python -m esptool --chip esp32s3 -p COM9 -b 921600 write_flash 0x0 moduleBox_full.bin

Состав папки:
  loader/              исходники загрузчика (проект IDF)
  bootloader.bin, partition-table.bin, bootldsd.bin, moduleBox.bin  - комплект для flash_kit
  moduleBox_full.bin   единый образ для производства
  UPDATE.FW            файл обновления для FTP
  bin2fw.c / .exe      упаковщик moduleBox.bin -> UPDATE.FW
  archive/             старые проекты загрузчика (IDF 4.4, чтение с карты), монофон, старые
                       образы - для прошивки не используются
